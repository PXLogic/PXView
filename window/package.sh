rm -rf package
mkdir package
cd package
cp ../install.dir/bin/PXView.exe .
cp -r ../install.dir/share/PXView/* .
cp -r ../install.dir/share/libsigrokdecode/* .
cp -r ../python/* .
../window/copy-deps.sh PXView.exe /mingw64
#qt
mkdir plugins
cp -r /mingw64/share/qt6/plugins/* .
../window/copy-deps.sh imageformats/qsvg.dll /mingw64
../window/copy-deps.sh imageformats/qjpeg.dll /mingw64
#python: extract stdlib from zip to lib/python3.14/ so Python can load .pyc without zlib
mkdir -p lib/python3.14
unzip -q python314.zip -d lib/python3.14/
cp /mingw64/lib/python3.14/lib-dynload/*.pyd lib/python3.14/

# webui (Vite web client)
if [ -d ../web/dist ]; then
    mkdir -p webui
    cp -r ../web/dist/* webui/
fi
