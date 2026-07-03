import socket
import cv2
import numpy as np
import time
import threading
import requests
from collections import deque
from datetime import datetime
from flask import Flask, Response, jsonify
from ultralytics import YOLO

# =========================
# 配置区
# =========================
UDP_PORT = 9999
MODEL_PATH = "/Users/pvpproof/vision-gateway/best.pt"
LOCAL_PORT = 8090

PUBLIC_BASE_URL = "http://101.132.116.112:8081"
PHP_EVENT_URL = "http://www.psylovecl.com/helmet_saver/api/upload_event.php"
DEVICE_ID = "esp32-p4-yolo-01"

NO_HELMET_CLASS_NAME = "No-Helmet"

CONF_THRESHOLD = 0.60
MIN_BOX_AREA = 4000

WINDOW_SIZE = 6
TRIGGER_RATIO = 0.20
CHECK_INTERVAL = 0.08
CAPTURE_COOLDOWN_SECONDS = 6
YOLO_IMGSZ = 480
WEB_JPEG_QUALITY = 70
FRAME_STALE_SECONDS = 1.0

app = Flask(__name__)


class UDPYoloGateway:
    def __init__(self, model_path, udp_port):
        print("🚀 正在初始化 YOLO 模型...")
        self.model = YOLO(model_path)
        self.udp_port = udp_port

        self.output_frame = None
        self.latest_snapshot = None
        self.frozen_snapshot = None
        self.frozen_snapshot_ts = 0
        self.preview_frame = None
        self.latest_detection_summary = None
        self.latest_event = None
        self.last_trigger_ts = 0

        self.latest_raw_frame = None
        self.latest_raw_frame_id = -1
        self.last_processed_frame_id = -1
        self.latest_raw_frame_ts = 0

        self.lock = threading.Lock()
        self.is_running = True

        self.recv_thread = threading.Thread(target=self._udp_receive_loop, daemon=True)
        self.recv_thread.start()

        self.yolo_thread = threading.Thread(target=self._yolo_loop, daemon=True)
        self.yolo_thread.start()

        self.judge_thread = threading.Thread(target=self._judge_loop, daemon=True)
        self.judge_thread.start()

    def _class_name_from_id(self, cls_id: int):
        if isinstance(self.model.names, dict):
            return self.model.names.get(cls_id, str(cls_id))
        if isinstance(self.model.names, list):
            return self.model.names[cls_id] if 0 <= cls_id < len(self.model.names) else str(cls_id)
        return str(cls_id)

    def _extract_light_summary(self, results):
        detections = []
        labels = []

        if not results:
            return {"ts": time.time(), "detections": [], "labels": []}

        result = results[0]
        boxes = result.boxes
        if boxes is None or boxes.cls is None:
            return {"ts": time.time(), "detections": [], "labels": []}

        cls_ids = boxes.cls.tolist()
        confs = boxes.conf.tolist() if boxes.conf is not None else []
        xyxy = boxes.xyxy.tolist() if boxes.xyxy is not None else []

        for index, cls_id in enumerate(cls_ids):
            cls_id = int(cls_id)
            label = self._class_name_from_id(cls_id)
            conf = float(confs[index]) if index < len(confs) else 0.0

            area = 0
            if index < len(xyxy):
                x1, y1, x2, y2 = xyxy[index]
                area = max(0, x2 - x1) * max(0, y2 - y1)

            labels.append(label)
            detections.append({
                "label": label,
                "conf": conf,
                "area": area,
            })

        return {"ts": time.time(), "detections": detections, "labels": labels}

    def _has_valid_no_helmet(self, summary):
        for detection in summary.get("detections", []):
            if detection["label"] != NO_HELMET_CLASS_NAME:
                continue
            if detection["conf"] < CONF_THRESHOLD:
                continue
            if detection["area"] < MIN_BOX_AREA:
                continue
            return True
        return False

    def _send_event(self, event):
        max_attempts = 3
        last_error = None

        for attempt in range(1, max_attempts + 1):
            try:
                response = requests.post(PHP_EVENT_URL, json=event, timeout=8)
                if response.ok:
                    try:
                        result = response.json()
                    except Exception:
                        result = {"raw": response.text}
                    print(f"☁️ 事件发送成功(第{attempt}次): {event['captured_at']} -> {result}")
                    return True

                last_error = f"HTTP {response.status_code}: {response.text}"
                print(f"⚠️ 事件发送失败(第{attempt}次) {last_error}")
            except Exception as error:
                last_error = str(error)
                print(f"⚠️ 事件发送异常(第{attempt}次): {error}")

            if attempt < max_attempts:
                time.sleep(1.0 * attempt)

        print(f"❌ 事件最终发送失败: {event['captured_at']} -> {last_error}")
        return False

    def _judge_loop(self):
        history = deque(maxlen=WINDOW_SIZE)
        last_summary_ts = 0

        while self.is_running:
            time.sleep(CHECK_INTERVAL)

            with self.lock:
                summary = self.latest_detection_summary

            if not summary:
                continue

            summary_ts = summary.get("ts", 0)
            if summary_ts == last_summary_ts:
                continue
            last_summary_ts = summary_ts

            hit = self._has_valid_no_helmet(summary)
            history.append(1 if hit else 0)

            if len(history) < WINDOW_SIZE:
                continue

            ratio = sum(history) / len(history)
            now = time.time()

            if ratio < TRIGGER_RATIO:
                continue
            if (now - self.last_trigger_ts) < CAPTURE_COOLDOWN_SECONDS:
                continue

            event = {
                "device_id": DEVICE_ID,
                "event_type": "no_helmet",
                "captured_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "labels": summary.get("labels", []),
                "snapshot_url": f"{PUBLIC_BASE_URL}/snapshot",
                "stream_url": f"{PUBLIC_BASE_URL}/stream",
            }

            with self.lock:
                if self.latest_snapshot is not None:
                    self.frozen_snapshot = self.latest_snapshot
                    self.frozen_snapshot_ts = now
                self.latest_event = {
                    "ok": True,
                    "type": event["event_type"],
                    "captured_at": event["captured_at"],
                    "labels": event["labels"],
                    "snapshot_url": event["snapshot_url"],
                    "stream_url": event["stream_url"],
                    "ratio": ratio,
                }

            self.last_trigger_ts = now
            history.clear()

            print(f"🟡 检测到异常：No-Helmet 命中比例={ratio:.2f}")
            self._send_event(event)

    def _store_latest_frame(self, frame, frame_id):
        with self.lock:
            self.latest_raw_frame = frame
            self.latest_raw_frame_id = frame_id
            self.latest_raw_frame_ts = time.time()

    def _process_yolo(self, frame):
        results = self.model(frame, verbose=False, imgsz=YOLO_IMGSZ)
        annotated_frame = results[0].plot()
        summary = self._extract_light_summary(results)

        success, buffer = cv2.imencode(
            ".jpg",
            annotated_frame,
            [int(cv2.IMWRITE_JPEG_QUALITY), WEB_JPEG_QUALITY],
        )

        with self.lock:
            self.preview_frame = annotated_frame.copy()
            if success:
                encoded = buffer.tobytes()
                self.output_frame = encoded
                self.latest_snapshot = encoded
            self.latest_detection_summary = summary

    def _udp_receive_loop(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", self.udp_port))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024 * 2)
        sock.settimeout(0.5)

        print(f"🚀 正在监听 UDP 端口 {self.udp_port}，等待 ESP32-P4 推流...")
        frame_buffer = {}

        while self.is_running:
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                self._cleanup_stale_buffers(frame_buffer)
                continue
            except Exception as error:
                print(f"⚠️ UDP 接收异常: {error}")
                continue

            if len(data) < 6:
                continue

            frame_id = (data[0] << 8) | data[1]
            chunk_idx = data[2]
            total_chunks = data[3]
            payload_len = (data[4] << 8) | data[5]

            if len(data) < 6 + payload_len:
                continue

            payload = data[6:6 + payload_len]

            if frame_id not in frame_buffer:
                frame_buffer[frame_id] = {
                    "total": total_chunks,
                    "chunks": {},
                    "ts": time.time(),
                }

            frame_buffer[frame_id]["chunks"][chunk_idx] = payload

            if len(frame_buffer[frame_id]["chunks"]) != total_chunks:
                self._cleanup_stale_buffers(frame_buffer)
                continue

            try:
                jpeg_data = b"".join(
                    frame_buffer[frame_id]["chunks"][index]
                    for index in range(total_chunks)
                )
            except KeyError:
                self._cleanup_stale_buffers(frame_buffer)
                continue

            np_arr = np.frombuffer(jpeg_data, np.uint8)
            frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

            current_ts = frame_buffer[frame_id]["ts"]
            keys_to_delete = [
                key for key, value in frame_buffer.items()
                if value["ts"] <= current_ts
            ]
            for key in keys_to_delete:
                del frame_buffer[key]

            self._cleanup_stale_buffers(frame_buffer)

            if frame is not None:
                self._store_latest_frame(frame, frame_id)

        sock.close()

    def _cleanup_stale_buffers(self, frame_buffer):
        now = time.time()
        stale_keys = [
            key for key, value in frame_buffer.items()
            if (now - value["ts"]) > FRAME_STALE_SECONDS
        ]
        for key in stale_keys:
            del frame_buffer[key]

    def _yolo_loop(self):
        while self.is_running:
            frame_to_process = None
            frame_id = -1

            with self.lock:
                frame_age = time.time() - self.latest_raw_frame_ts if self.latest_raw_frame_ts else None
                if (
                    self.latest_raw_frame is not None
                    and self.latest_raw_frame_id != self.last_processed_frame_id
                    and (frame_age is None or frame_age <= FRAME_STALE_SECONDS)
                ):
                    frame_to_process = self.latest_raw_frame.copy()
                    frame_id = self.latest_raw_frame_id
                    self.last_processed_frame_id = frame_id

            if frame_to_process is None:
                time.sleep(0.01)
                continue

            self._process_yolo(frame_to_process)

    def get_encoded_frame(self):
        while self.is_running:
            with self.lock:
                frame = self.output_frame

            if frame is not None:
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
                )
            time.sleep(0.03)

    def get_snapshot(self):
        with self.lock:
            if self.frozen_snapshot is not None and (time.time() - self.frozen_snapshot_ts) <= CAPTURE_COOLDOWN_SECONDS:
                return self.frozen_snapshot
            return self.latest_snapshot


gateway = UDPYoloGateway(MODEL_PATH, UDP_PORT)


@app.route("/stream")
def video_feed():
    return Response(gateway.get_encoded_frame(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/snapshot")
def snapshot():
    image_bytes = gateway.get_snapshot()
    if image_bytes is None:
        return jsonify({"ok": False, "message": "暂无图像"}), 404
    return Response(image_bytes, mimetype="image/jpeg")


@app.route("/latest_event")
def latest_event():
    with gateway.lock:
        event = gateway.latest_event
    if event is not None:
        return jsonify(event)
    return jsonify({"ok": False, "message": "暂无异常事件"})


@app.route("/")
def index():
    return "<h1>YOLO UDP 网关运行中</h1>"


if __name__ == "__main__":
    print(f"✅ 服务就绪！Web API 端口: {LOCAL_PORT}")

    flask_thread = threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=LOCAL_PORT, debug=False, use_reloader=False),
        daemon=True,
    )
    flask_thread.start()

    print("📺 本地窗口已就绪 (选中窗口后，按键盘 'q' 键退出)...")

    while True:
        with gateway.lock:
            frame = gateway.preview_frame

        if frame is not None:
            cv2.imshow("ESP32-P4 YOLO Stream", frame)

        if cv2.waitKey(10) & 0xFF == ord('q'):
            print("🛑 正在关闭...")
            gateway.is_running = False
            break

    cv2.destroyAllWindows()
