# BKHN Auto Survey Evaluation (DevExpress Bypass)

Userscript giúp tự động hóa quá trình điền khảo sát/đánh giá học phần cuối kỳ trên các hệ thống cổng thông tin đào tạo của H đỏ: `ctt-sis.hxxx.edu.vn`.

Dự án này được tối ưu hóa đặc biệt nhằm bypass qua cơ chế render giao diện giả lập (Fake DOM Radio Buttons) chằng chịt của thư viện **DevExpress** và tích hợp một bảng điều khiển ngay trên giao diện web.

---

## ✨ Tính năng nổi bật

1. **In-Page Control Panel (Shadow DOM):**
   - Bảng điều khiển tích hợp nổi trên góc màn hình với phong cách thiết kế **Dark Glassmorphism** hiện đại.
   - Sử dụng **Shadow DOM** cô lập CSS hoàn toàn, loại bỏ 100% khả năng bị xung đột hay lỗi vỡ giao diện với thư viện Bootstrap cũ của hệ thống trường.
   - Giao diện hỗ trợ co giãn (Minimize/Expand) linh hoạt để tránh che tầm nhìn của người dùng kèm console log mini hiển thị trạng thái trực tiếp.

2. **Quản lý trạng thái (State Management):**
   - Đồng bộ hóa cấu hình người dùng (Bật/tắt quét, độ trễ quét, độ trễ reload, chiến thuật) bằng API `GM_setValue` và `GM_getValue`.
   - Giữ nguyên cấu hình bạn thiết lập sau khi reload trang hoặc tự chuyển sang phiếu khảo sát môn học khác.

3. **3 Chiến thuật khảo sát linh hoạt:**
   - **Max Điểm:** Tick các đáp án cao nhất (4-5 sao).
   - **Thực tế (Realistic):** Trộn ngẫu nhiên Khá (Good) và Giỏi/Xuất sắc (Excellent) theo tỷ lệ thực tế **75% Xuất sắc - 25% Khá** (Khuyên dùng để bộ dữ liệu trông tự nhiên nhất).
   - **Ngẫu nhiên:** Lựa chọn ngẫu nhiên hoàn toàn một trong các đáp án khả dụng.
   - *Tất cả chiến thuật đều bảo toàn logic điền các câu hỏi đặc thù (thời gian tự học, mức độ tăng thêm,...) nhờ cơ chế lọc từ khóa thông minh.*

4. **Khả năng chống chịu cao (Robustness):**
   - Nhận diện nhóm câu hỏi DevExpress bằng Regular Expression, không lo bị vỡ nếu hệ thống cập nhật nhẹ cấu trúc ID động của HTML.

---

## 🚀 Hướng dẫn Cài đặt & Setup

### Bước 1: Cài đặt Extension quản lý Userscript
Trước hết, bạn cần cài đặt một extension quản lý userscript trên trình duyệt của mình:
- **Chrome / Edge / Opera / Brave:** Cài đặt [Tampermonkey](https://chrome.google.com/webstore/detail/tampermonkey/dhdgffkkebhmkfjojejmpbldmpobfkfo) (Khuyên dùng) hoặc [Violentmonkey](https://violentmonkey.github.io/).
- **Firefox:** Cài đặt [Tampermonkey](https://addons.mozilla.org/vi/firefox/addon/tampermonkey/) hoặc [Greasemonkey](https://addons.mozilla.org/vi/firefox/addon/greasemonkey/).

### Bước 2: Tạo script mới trong Tampermonkey
1. Nhấp vào biểu tượng Tampermonkey trên thanh công cụ của trình duyệt và chọn **"Tạo thư mục mới" / "Create a new script..."**.
2. Xóa toàn bộ nội dung mã nguồn mặc định có sẵn trong khung editor.

### Bước 3: Copy & Paste mã nguồn
1. Mở tệp [bkhn-auto-survey.user.js](bkhn-auto-survey.user.js) trong dự án này.
2. Sao chép (Copy) toàn bộ mã nguồn của tệp đó.
3. Dán (Paste) mã nguồn vào khung soạn thảo của Tampermonkey.
4. Nhấn **Ctrl + S** (hoặc chọn menu **File** -> **Save**) để lưu lại script.

---

## 🛠️ Hướng dẫn sử dụng chi tiết

Khi bạn truy cập vào các trang khảo sát có liên kết dạng:
- `https://ctt-sis.hust.edu.vn/Surveys/...`
- `https://ctt-daotao.hust.edu.vn/Surveys/...`

Bạn sẽ thấy một bảng điều khiển nổi xuất hiện ở góc dưới bên phải màn hình:

- **Bật/Tắt Tự động quét (Tự động chạy):**
  - **BẬT (Mặc định):** Ngay khi phát hiện phiếu khảo sát, script tự điền các câu hỏi theo chiến thuật đang chọn, tự động gửi phiếu lên máy chủ, và tự động reload tải trang tiếp theo sau độ trễ cấu hình.
  - **TẮT:** Script sẽ tạm dừng quét. Bạn có thể sử dụng nút **"Điền Khảo Sát Ngay"** để điền nhanh các đáp án mà không lo script tự động gửi hay chuyển trang.
- **Chiến thuật khảo sát:** Chọn chiến thuật đánh giá phù hợp ("Max Điểm", "Thực tế", "Ngẫu nhiên"). Các thay đổi sẽ được lưu trữ tự động.
- **Độ trễ quét (scanIntervalMs):** Tần suất (mili-giây) mà script quét tìm cấu trúc form. Mặc định là `2000`ms.
- **Độ trễ Reload (reloadWaitMs):** Thời gian chờ (mili-giây) sau khi click gửi form khảo sát trước khi reload để chuyển sang môn tiếp theo. Mặc định là `4000`ms (Thời gian thích hợp để server ASP.NET xử lý hoàn tất postback).

---

## ⚠️ Lưu ý miễn trừ trách nhiệm (Disclaimer)

Dự án này được phát triển cho mục đích học tập, nghiên cứu cách bypass các thư viện UI phức tạp (DevExpress) và hỗ trợ tự động hóa cá nhân. Người sử dụng tự chịu mọi trách nhiệm liên quan đến các kết quả đánh giá khảo sát của cá nhân trên hệ thống của trường học.
