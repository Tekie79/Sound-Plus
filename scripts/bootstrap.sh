#!/bin/zsh
set -eu
cd "${0:A:h:h}"
python3 -m pip install --target .tools cmake==3.31.6 ninja==1.11.1.3
mkdir -p vendor
archive=$(mktemp /tmp/soundplus-juce.XXXXXX)
trap 'rm -f "$archive"' EXIT
curl -fL --retry 2 https://codeload.github.com/juce-framework/JUCE/tar.gz/refs/tags/8.0.6 -o "$archive"
echo "95752f8e8b37d802b4c10182bc757de7d88f0c8899e879afa8798589be306a11  $archive" | shasum -a 256 -c -
tar -xzf "$archive" -C vendor
python3 scripts/fetch_runtime.py
