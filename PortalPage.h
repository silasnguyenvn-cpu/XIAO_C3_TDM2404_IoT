#pragma once
#include <Arduino.h>
const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="vi"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>XIAO GPS · Cấu hình</title><style>
:root{font-family:system-ui,sans-serif;color:#16302f;background:#eff5f3}*{box-sizing:border-box}
body{margin:0 auto;padding:22px 16px 42px;max-width:640px}header{margin:12px 0 24px}
.tag{font-size:12px;color:#476d64;letter-spacing:.12em}h1{font-size:29px;margin:9px 0}p{line-height:1.55;color:#48605b}
section{background:white;border:1px solid #dae7e0;border-radius:18px;padding:20px;margin:16px 0}
h2{font-size:18px;margin:0 0 15px}label{display:block;margin:14px 0 7px;font-weight:600;font-size:14px}
input:not([type=checkbox]){font:inherit;width:100%;padding:12px;border:1px solid #bdcfc8;border-radius:10px;background:#fbfdfc}
input:focus{outline:2px solid #37927a}small{display:block;color:#5f746d;margin-top:5px;line-height:1.5}
button{font:inherit;border:0;border-radius:11px;padding:13px 18px;cursor:pointer;background:#176950;color:white}
.secondary{background:#e5efe9;color:#174e3e}.row{display:flex;gap:10px;flex-wrap:wrap}.row button{flex:1}
.status{font-size:14px;white-space:pre-wrap;line-height:1.7}#notice{color:#ac3128;white-space:pre-wrap}
details{margin-top:16px}summary{cursor:pointer;color:#345c4f}a{color:#176950}button:disabled{opacity:.5}
</style><header><div class="tag">XIAO · EG800K · 4G</div><h1>Thiết bị định vị</h1>
<p>Cấu hình ngay trên điện thoại. Vị trí được gửi lên cloud qua SIM 4G.</p></header>
<section><h2>Trạng thái thiết bị</h2><div id="status" class="status">Đang đọc trạng thái…</div>
<p id="map"></p><small id="result"></small><div class="row"><button class="secondary" type="button" onclick="action('upload')">Gửi vị trí lên cloud</button>
<button class="secondary" type="button" onclick="action('test')">Gửi SMS thử</button></div>
<small>SMS thử gửi một tin tới số SOS 1. Nút SOS trên mạch gửi tới các số đã lưu.</small></section>
<form id="settings"><section><h2>Circuit Digest Cloud</h2>
<label><input type="checkbox" id="cloud" name="cloud" value="1"> Bật gửi vị trí định kỳ</label>
<label for="device">Device ID</label><input id="device" name="device" maxlength="64" placeholder="Nhập đúng Device ID trên GeoLinker">
<label for="key">API key</label><input id="key" name="key" type="password" maxlength="160" autocomplete="new-password" placeholder="Nhập key GeoLinker">
<small id="keynote">Key được lưu trên thiết bị và không hiển thị lại.</small>
<label for="interval">Gửi mỗi bao nhiêu giây?</label><input id="interval" name="interval" type="number" min="30" max="3600" value="60" required>
</section><section><h2>SIM và Internet 4G</h2>
<label for="apn">APN của SIM</label><input id="apn" name="apn" maxlength="63" required>
<small>Viettel thông thường dùng v-internet. SIM doanh nghiệp/IoT có thể dùng APN riêng do nhà mạng cung cấp.</small>
<details><summary>Tài khoản APN (nếu nhà mạng yêu cầu)</summary>
<label for="user">Tên đăng nhập APN</label><input id="user" name="user" maxlength="63" autocomplete="off">
<label for="pass">Mật khẩu APN</label><input id="pass" name="pass" type="password" maxlength="63" autocomplete="new-password" placeholder="Để trống để giữ mật khẩu đã lưu">
<label><input type="checkbox" name="clearpass" value="1"> Xóa mật khẩu APN đã lưu</label></details>
</section><section><h2>Người nhận SOS</h2>
<p>Giữ nút SOS 3 giây để gửi SMS. Khi chưa có GPS, tin nhắn vẫn báo cần giúp đỡ.</p>
<label for="phone1">Số SOS 1</label><input id="phone1" name="phone1" type="tel" maxlength="16" required>
<label for="phone2">Số SOS 2 · tùy chọn</label><input id="phone2" name="phone2" type="tel" maxlength="16">
<label for="phone3">Số SOS 3 · tùy chọn</label><input id="phone3" name="phone3" type="tel" maxlength="16">
</section><section><h2>Wi-Fi cấu hình</h2><label for="wifi">Đổi mật khẩu Wi-Fi</label>
<input id="wifi" name="wifi" type="password" minlength="8" maxlength="63" autocomplete="new-password" placeholder="Để trống để giữ mật khẩu hiện tại">
<small>Mật khẩu mới áp dụng ở lần mở cấu hình tiếp theo. Giữ CONFIG 3 giây để mở lại trang này.</small>
</section><p id="notice" role="status"></p><div class="row"><button id="save" type="submit">Lưu cấu hình</button>
<button type="button" class="secondary" onclick="action('close')">Đóng Wi-Fi</button></div>
<small>Sau khi lưu, chờ thông báo rồi đóng Wi-Fi để tiếp tục sử dụng bình thường.</small></form>
<script>
let token='';
const el=id=>document.getElementById(id);
async function read(){try{
 const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error('Không đọc được thiết bị');
 const s=await r.json();
 el('status').textContent='SIM: '+s.sim+' · Mạng: '+s.network+'\nGPS: '+s.gps+'\nPin: '+s.battery+' V (ước lượng)\nCloud: '+s.cloud+'\nSMS: '+s.sms+'\nTác vụ: '+s.job;
 el('result').textContent=s.event;
 el('map').replaceChildren();if(s.map){const a=document.createElement('a');a.href=s.map;a.textContent=s.fresh?'Mở vị trí GPS mới nhất':'Mở vị trí đã biết trước đó';a.target='_blank';a.rel='noopener';el('map').append(a);}
}catch(e){el('status').textContent='Mất kết nối thiết bị. Kiểm tra Wi-Fi cấu hình.';}}
async function action(name){
 if(name==='test'&&!confirm('Gửi một SMS thử đến số SOS 1 đã lưu?'))return;
 try{const r=await fetch('/api/action',{method:'POST',body:new URLSearchParams({token,action:name})});el('notice').textContent=await r.text();}catch(e){el('notice').textContent='Không kết nối được thiết bị.';}}
el('settings').addEventListener('submit',async e=>{e.preventDefault();el('save').disabled=true;
 try{const data=new URLSearchParams(new FormData(e.target));data.set('token',token);
 const r=await fetch('/api/save',{method:'POST',body:data});el('notice').textContent=await r.text();
 if(r.ok){el('key').value='';el('pass').value='';el('wifi').value='';el('keynote').textContent='Đã lưu key; để trống để giữ key hiện tại.';}}
 catch(e){el('notice').textContent='Không kết nối được thiết bị; kiểm tra trước khi lưu lại.';}finally{el('save').disabled=false;}});
(async()=>{try{const r=await fetch('/api/config',{cache:'no-store'});const c=await r.json();token=c.token;
 for(const k of ['device','apn','user','phone1','phone2','phone3','interval'])el(k).value=c[k];
 el('cloud').checked=c.cloud;if(c.hasKey)el('keynote').textContent='Đã lưu key; để trống để giữ key hiện tại.';
}catch(e){el('notice').textContent='Không tải được cấu hình; tải lại trang.';}await read();setInterval(read,4000);})();
</script></html>)HTML";
