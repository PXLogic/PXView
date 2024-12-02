#!/bin/bash

# Configurations
BUNDLE='deploy_mac/PXView.app'
PYTHON_VER='3.13'
CODESIGN_SIGNATURE="-" # Change to your signature identification

# Configure, build, install
# Ninja is used because it uses multithreading by default
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=deploy_mac
cmake --build build --target all
cmake --build build --target install

macdeployqt ${BUNDLE}

# Python framework path (as of Python 3.13 from Homebrew on 2024/12/2) is not recognized by macdeployqt
# Manually copy frameworks and fix library reference path
cp -R /opt/homebrew/opt/python@${PYTHON_VER}/Frameworks/Python.framework ${BUNDLE}/Contents/Frameworks
install_name_tool \
    -change /opt/homebrew/opt/python@${PYTHON_VER}/Frameworks/Python.framework/Versions/${PYTHON_VER}/Python \
    '@executable_path/../Frameworks/Python.Framework/Versions/Current/Python' ${BUNDLE}/Contents/MacOS/PXView

# Fix package signature after deploying.
codesign --force --deep --sign ${CODESIGN_SIGNATURE} ${BUNDLE}
