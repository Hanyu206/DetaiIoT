import os
from dotenv import load_dotenv
from ultralytics import YOLO
import cv2
import requests
import time
import threading
from flask import Flask, Response
from collections import deque

load_dotenv()
TOKEN = os.getenv("BLYNK_AUTH_TOKEN")
BASE_URL = "https://sgp1.blynk.cloud/external/api/update"

# ==================== CẤU HÌNH 2 HƯỚNG ====================
VIDEO_A = "VideohuongA.mp4"     
VIDEO_B = "VideohuongB.mp4"  

NGUONG_MAT_DAU = 10   
NGUONG_XAC_NHAN = 7
KHOANG_CACH_GUI = 1  

# ==================== TRẠNG THÁI DÙNG CHUNG GIỮA 2 LUỒNG ====================
khoa_frame = threading.Lock()
khung_hinh = {"A": None, "B": None}   # Khung hình mới nhất của mỗi hướng, phục vụ Flask

khoa_uu_tien = threading.Lock()
trang_thai_uu_tien = {"A": False, "B": False}   # Cờ ưu tiên hiện tại của từng hướng

app = Flask(__name__)


# ==================== LUỒNG VIDEO WEB (Flask) - tách riêng cho từng hướng ====================
def tao_luong_video(lane):
    while True:
        with khoa_frame:
            frame = khung_hinh[lane]
            if frame is None:
                time.sleep(0.01)
                continue
            ret, buffer = cv2.imencode('.jpg', frame)
            khung_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + khung_bytes + b'\r\n')
        time.sleep(0.04)


@app.route('/video_feed_a')
def video_feed_a():
    return Response(tao_luong_video("A"), mimetype='multipart/x-mixed-replace; boundary=frame')


@app.route('/video_feed_b')
def video_feed_b():
    return Response(tao_luong_video("B"), mimetype='multipart/x-mixed-replace; boundary=frame')


def _snapshot(lane):
    with khoa_frame:
        frame = khung_hinh[lane]
        if frame is None:
            return Response(status=204)
        ret, buffer = cv2.imencode('.jpg', frame)
        khung_bytes = buffer.tobytes()
    return Response(khung_bytes, mimetype='image/jpeg')


@app.route('/snapshot_a')
def snapshot_a():
    return _snapshot("A")


@app.route('/snapshot_b')
def snapshot_b():
    return _snapshot("B")


def chay_flask():
    app.run(host='0.0.0.0', port=5000, debug=False, use_reloader=False)


# ==================== GỬI DỮ LIỆU LÊN BLYNK (bất đồng bộ, không làm giật khung hình) ====================
def gui_api_async(url, nhan=""):
    def _gui():
        t0 = time.time()
        try:
            requests.get(url, timeout=1.5)
            if nhan:
                print(f"-> {nhan} | Do tre Blynk: {(time.time() - t0) * 1000:.1f} ms")
        except Exception as e:
            print(f"Loi ket noi Blynk ({nhan}):", e)
    threading.Thread(target=_gui, daemon=True).start()


def gui_so_xe(pin, so_xe, nhan):
    url = f"{BASE_URL}?token={TOKEN}&{pin}={so_xe}"
    gui_api_async(url, nhan)


def cap_nhat_uu_tien_len_blynk():
    # V2: 0 = khong uu tien | 1 = uu tien huong A | 2 = uu tien huong B
    # Neu ca 2 huong cung luc phat hien xe uu tien (hiem gap), uu tien A duoc gui truoc.
    with khoa_uu_tien:
        if trang_thai_uu_tien["A"]:
            val = 1
        elif trang_thai_uu_tien["B"]:
            val = 2
        else:
            val = 0
    url = f"{BASE_URL}?token={TOKEN}&V2={val}"
    gui_api_async(url, f"V2={val}")


# ==================== XỬ LÝ 1 HƯỚNG (chạy trong thread riêng) ====================
def xu_ly_huong(lane, video_path, pin_so_xe):
    # Mỗi hướng load model riêng (không dùng chung 1 instance giữa các thread)
    # để tránh xung đột khi 2 luồng cùng gọi model song song.
    model = YOLO("yolov8n.pt")
    model_uu_tien = YOLO("YOLO_xe_uu_tien.pt")

    cap = cv2.VideoCapture(video_path)
    lich_su_so_xe = deque(maxlen=5)
    so_frame_mat_dau = 0
    so_frame_phat_hien = 0
    lan_gui_cuoi = 0

    while cap.isOpened():
        t_start = time.time()
        ret, frame = cap.read()
        if not ret:
            # Hết video -> quay lại từ đầu để tiếp tục mô phỏng luồng liên tục
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            continue

        # 1. Nhận diện xe thông thường
        results = model(frame, classes=[2, 3, 5, 7], conf=0.3, verbose=False)
        so_xe = len(results[0].boxes)
        lich_su_so_xe.append(so_xe)
        so_xe_trung_binh = round(sum(lich_su_so_xe) / len(lich_su_so_xe))

        # 2. Nhận diện xe ưu tiên
        # 2. Nhận diện xe ưu tiên
        results_uu_tien = model_uu_tien(frame, conf=0.15, verbose=False)

        # Lọc bỏ khung quá lớn (chiếm >35% diện tích khung hình) — đây gần như
        # chắc chắn là dự đoán lỗi (model "mơ hồ" khi gặp cảnh khác miền dữ liệu train),
        # không phải 1 chiếc xe thật nào cả.
        dien_tich_khung_hinh = frame.shape[0] * frame.shape[1]
        khung_uu_tien_hop_le = []
        for box in results_uu_tien[0].boxes:
            x1, y1, x2, y2 = box.xyxy[0]
            dien_tich_box = float((x2 - x1) * (y2 - y1))
            if dien_tich_box <= 0.35 * dien_tich_khung_hinh:
                khung_uu_tien_hop_le.append(box)

        co_xe_uu_tien = len(khung_uu_tien_hop_le) > 0

        # 3. Thuật toán cờ khẩn cấp (riêng cho từng hướng, đồng bộ chung lên V2)
        with khoa_uu_tien:
            dang_uu_tien = trang_thai_uu_tien[lane]

        if co_xe_uu_tien:
            so_frame_mat_dau = 0
            if not dang_uu_tien:
                # Chưa bật cờ -> cần thấy liên tục NGUONG_XAC_NHAN khung hình mới xác nhận
                so_frame_phat_hien += 1
                if so_frame_phat_hien >= NGUONG_XAC_NHAN:
                    with khoa_uu_tien:
                        trang_thai_uu_tien[lane] = True
                    so_frame_phat_hien = 0
                    cap_nhat_uu_tien_len_blynk()
                    print(f"[CANH BAO][Huong {lane}] Phat hien xe uu tien")
        else:
            so_frame_phat_hien = 0   # Mất dấu 1 khung hình -> reset lại bộ đếm xác nhận
            if dang_uu_tien:
                so_frame_mat_dau += 1
                if so_frame_mat_dau >= NGUONG_MAT_DAU:
                    with khoa_uu_tien:
                        trang_thai_uu_tien[lane] = False
                    so_frame_mat_dau = 0
                    cap_nhat_uu_tien_len_blynk()
                    print(f"[THONG BAO][Huong {lane}] Het xe uu tien")

        # 4. Trích xuất độ trễ / FPS
        speed = results[0].speed
        latency_inference = speed['inference']
        t_total_ms = (time.time() - t_start) * 1000
        fps = 1000 / t_total_ms if t_total_ms > 0 else 0

        # 5. Vẽ giao diện hiển thị
        frame_ve = results[0].plot()

        # Vẽ thêm khung của model_uu_tien (màu hồng) để thấy rõ nó đang nhận nhầm vật gì
        for box in khung_uu_tien_hop_le:
            x1, y1, x2, y2 = map(int, box.xyxy[0])
            ten_lop = model_uu_tien.names[int(box.cls[0])]
            do_tin_cay = float(box.conf[0])
            cv2.rectangle(frame_ve, (x1, y1), (x2, y2), (255, 0, 255), 2)
            cv2.putText(frame_ve, f"UUTIEN:{ten_lop}{do_tin_cay:.2f}", (x1, y1 - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 0, 255), 2)

        with khoa_uu_tien:
            dang_uu_tien_hien_thi = trang_thai_uu_tien[lane]
        if dang_uu_tien_hien_thi:
            cv2.putText(frame_ve, "XE UU TIEN!", (20, 80),
                        cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 3)

        cv2.putText(frame_ve, f"Huong {lane} | Xe: {so_xe_trung_binh} | Latency: {latency_inference:.1f}ms | FPS: {fps:.1f}",
                    (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 255, 0), 2)

        with khoa_frame:
            khung_hinh[lane] = frame_ve.copy()
        cv2.imshow(f"He thong Giam sat AI - Huong {lane}", frame_ve)

        # Gửi định kỳ số xe lên Blynk
        now = time.time()
        if now - lan_gui_cuoi >= KHOANG_CACH_GUI:
            gui_so_xe(pin_so_xe, so_xe_trung_binh, f"Huong {lane}")
            lan_gui_cuoi = now

        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cap.release()


if __name__ == "__main__":
    threading.Thread(target=chay_flask, daemon=True).start()

    luong_a = threading.Thread(target=xu_ly_huong, args=("A", VIDEO_A, "V0"), daemon=True)
    luong_b = threading.Thread(target=xu_ly_huong, args=("B", VIDEO_B, "V5"), daemon=True)
    luong_a.start()
    luong_b.start()

    luong_a.join()
    luong_b.join()
    cv2.destroyAllWindows()
