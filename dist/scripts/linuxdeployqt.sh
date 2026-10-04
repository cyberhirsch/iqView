#!/usr/bin/bash

wget 'https://sourceforge.net/projects/qt5ct/files/latest/download'
tar xf download
cd qt5ct*
qmake
sudo make install
cd ..

if [ -n "$1" ]; then
    VERSION="$1"
else
    VERSION=$(grep -m1 '^CMAKE_PROJECT_VERSION:' build/CMakeCache.txt | cut -d= -f2)
fi

# echo VERSION was set to $VERSION
# if [[ $2 == *"-extra-plugins"* ]]; then
#         PLUGINS=$2
# fi

wget -c -nv "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
chmod a+x linuxdeployqt-continuous-x86_64.AppImage

mkdir -p bin/appdir/usr
DESTDIR="$PWD/bin/appdir" cmake --install build --prefix /usr
# Copy AI scripts to staging
cp -r scripts bin/appdir/usr/bin/scripts
cp dist/linux/hicolor/scalable/apps/io.github.cyberhirsch.iqView.svg bin/appdir/
cd bin
rm iqview
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib/
../linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/io.github.cyberhirsch.iqView.desktop -appimage -updateinformation="gh-releases-zsync|cyberhirsch|iqView|latest|iqView-*x86_64.AppImage.zsync" -extra-plugins=styles/libqt5ct-style.so,platformthemes/libqt5ct.so

if [ -n "$1" ]; then
    mv *.AppImage iqView-nightly-$1-x86_64.AppImage
else
    mv *.AppImage iqView-$VERSION-x86_64.AppImage
fi
rm -r appdir
