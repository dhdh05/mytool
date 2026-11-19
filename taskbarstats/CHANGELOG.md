# Changelog

## v1.1

### Sửa lỗi
- **Màn hình phụ không hiện thông số**: trước đây app chỉ gắn vào taskbar của màn chính (`Shell_TrayWnd`). Giờ mỗi taskbar, kể cả taskbar màn phụ (`Shell_SecondaryTrayWnd`), đều có ô thông số riêng. Cắm hay rút màn thì ô thông số tự hiện hoặc tự ẩn. Explorer khởi động lại thì app tự gắn lại.
- **Mất thông số khi mở Search (Win+Q) / Start**: khi mở các khung này, Windows đẩy taskbar lên tầng cao hơn mọi cửa sổ "luôn ở trên" nên ô thông số bị che. Giờ ô thông số là cửa sổ con của taskbar nên luôn nằm trên taskbar.
- **Full screen**: chỉ ẩn ô thông số trên màn đang có app full screen, màn còn lại vẫn hiện. Start, Search, Notification Center và Widgets không còn bị nhận nhầm là app full screen.

### Tối ưu
- **Đọc cảm biến ở luồng riêng** (độ ưu tiên thấp): CPU, RAM, GPU, nhiệt độ SSD, pin Bluetooth không còn chạy trên luồng giao diện. Taskbar không bị khựng kể cả khi đọc cảm biến chậm, ví dụ lúc quét thiết bị Bluetooth.
- **Không vẽ lại khi không có gì thay đổi**: so dấu vân tay nội dung + vị trí, giống lần trước thì bỏ qua.
- **Quét taskbar khi cần**: lúc cắm/rút màn, lúc Explorer khởi động lại, hoặc 5 giây một lần để dự phòng, thay vì mỗi giây.
- Số liệu mới được vẽ ngay khi đọc xong, không phải chờ tới tick tiếp theo.
- Khi thoát, app dừng luồng cảm biến và đóng PDH cùng handle ổ đĩa.

### Build
- Thêm `taskbarstats.rc` + `taskbarstats.manifest`, và `build.bat` đã có sẵn bước nhúng manifest. Nếu tự build bằng tay, nhớ chạy `windres` (xem README).
