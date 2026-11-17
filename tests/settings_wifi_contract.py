"""Страница «Сеть» без адаптера не сдвигает DNS; блок Wi-Fi появляется только с wlan0."""
from pathlib import Path
settings = Path("user/settings.c").read_text()
desktop = Path("user/desktop.c").read_text()
assert "#define FIELD_Y 378" in settings
assert "#define ACTION_Y 432" in settings
assert "wlan_present()" in settings
assert "if (page==0 && wlan_present() && wifi_link())" in settings
assert "focus==1" in settings and "focus==2 ? wifi_pass : dns" in settings
enter = settings.split("e.key=='\\n'", 1)[1]
assert "set_dns" in enter[:400]
assert "tray_has_wifi()" in desktop
assert "x < t.x + 34" in desktop
print("settings wifi contract: PASS")
