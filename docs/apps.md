# Приложения рабочего стола

Рабочий стол читает каталог приложений из `/usr/share/applications`. Для нового приложения положите исполняемый файл в `/bin` или `/ext/bin`, а рядом создайте файл `/usr/share/applications/<id>.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=My App
Exec=myapp
Icon=myapp
Keywords=пример поиск
X-MyOS-Favorite=false
```

`Exec` задаёт имя файла из `/bin` или `/ext/bin` (например, `myapp`, `/bin/myapp` или `/ext/bin/myapp`). Оно запускается без аргументов. Обязательны `Name` и `Exec`; остальные поля необязательны. Если `Icon` не совпадает со встроенным значком, оболочка нарисует стандартный значок с первой буквой названия. `Keywords` и `Comment` участвуют в поиске. Для закрепления приложения в доке укажите `X-MyOS-Favorite=true`.

При сборке стандартные записи из `apps/*.desktop` устанавливаются в `/usr/share/applications`; после загрузки оболочка автоматически показывает записи, добавленные в этот каталог.

