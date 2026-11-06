# Thiết kế mạch định vị GPS

Mã nguồn firmware 3.1.3 cho XIAO ESP32-C3 và modem Quectel EG800K. Thiết bị đọc GPS, gửi vị trí qua 4G lên GeoLinker và hỗ trợ SOS qua SMS.

## Nạp chương trình

1. Nếu tải bằng **Code → Download ZIP**, giải nén và đổi tên thư mục chứa tệp .ino thành `XIAO_C3_TDM2404_IoT`.
2. Mở `XIAO_C3_TDM2404_IoT.ino` bằng Arduino IDE, chọn board **XIAO_ESP32C3** trong Arduino ESP32 core **3.3.12** và bật **USB CDC On Boot** để xem log qua USB.
3. Nạp chương trình. Giữ **CONFIG** khoảng 3 giây để mở Wi-Fi cấu hình. Kết nối Wi-Fi **XIAO-GPS-…** (mật khẩu khởi tạo `gps123456`) và mở `http://192.168.4.1`.
4. Nhập số nhận SOS, APN và thông số GeoLinker nếu dùng cloud. Đổi mật khẩu Wi-Fi cấu hình khi thiết lập thiết bị.

## SMS SOS

- Giữ nút **SOS** khoảng 3 giây để gửi SMS tới các số nhận đã lưu.
- Để bật yêu cầu vị trí qua SMS từ xa, điền số được phép gửi lệnh vào biến `authorizedSender` trong hàm `remoteSenderAllowed()` trước khi biên dịch. Giá trị để trống sẽ từ chối yêu cầu từ xa. Giữ bản cấu hình có số cá nhân ở máy của bạn.
- Số được phép gửi lệnh SOS và các số nhận kết quả là hai thiết lập riêng.

Bản công khai để trống số điện thoại và sử dụng tọa độ mẫu cho các kiểm tra bộ phân tích GPS.
