#!/bin/bash
# Script pentru actualizarea iconului Pafari

echo "Actualizare icon Pafari..."

# Copiaza iconul nou
sudo cp /home/alxb421/Desktop/pafari/data/icons/hicolor/scalable/apps/applogo.svg /usr/local/share/icons/hicolor/scalable/apps/org.gnome.Pafari.svg

# Copiaza iconul symbolic nou
sudo cp /home/alxb421/Desktop/pafari/data/icons/hicolor/symbolic/apps/applogo-symbolic.svg /usr/local/share/icons/hicolor/symbolic/apps/org.gnome.Pafari-symbolic.svg

# Actualizeaza cache-ul de iconuri
sudo gtk-update-icon-cache -f /usr/local/share/icons/hicolor/

echo "Iconul a fost actualizat!"
echo "Daca aplicatia ruleaza, inchide-o si reporneste-o pentru a vedea iconul nou."

