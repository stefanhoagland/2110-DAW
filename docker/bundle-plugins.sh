#!/bin/sh
# Collect the free effect plugins that ship with the image into $1, plus an
# index.json the app reads to list them in + Plugin.
# Source: burns-audio-wam on npm (MIT License, Thomas Burns), the stock Web
# Audio Modules of sequencer.party. Only audio effects are included; its
# instruments, MIDI and video plugins don't fit a channel insert.
set -e
out=$1
version=0.2.54
mkdir -p "$out"
cd /tmp
npm pack --silent "burns-audio-wam@$version" >/dev/null
tar xzf "burns-audio-wam-$version.tgz"
src=package/dist/plugins
index=""
add() { # folder in the package, folder in the image, name shown in the menu
  cp -R "$src/$1" "$out/$2"
  index="$index${index:+,}{\"name\":\"$3\",\"url\":\"bundled-plugins/$2/index.js\"}"
}
add simpleEQ eq "Simple EQ"
add delay delay "Simple Delay"
add distortion distortion "Simple Distortion"
add reverb reverb "Microverb"
echo "[$index]" > "$out/index.json"
printf 'Plugins from burns-audio-wam %s by Thomas Burns, MIT License.\nhttps://github.com/boourns/burns-audio-wam\n' "$version" > "$out/NOTICE.txt"
