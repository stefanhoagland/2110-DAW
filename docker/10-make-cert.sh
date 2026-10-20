#!/bin/sh
# Create a self-signed HTTPS certificate on first start, unless one exists.
# CERT_HOSTNAMES: comma-separated names/IPs to put on the certificate,
# for example "tower.local,192.168.1.50".
set -e
mkdir -p /config/plugins # third-party plugins, served at /plugins/
dir=/config/certs
[ -s "$dir/cert.pem" ] && [ -s "$dir/key.pem" ] && exit 0
mkdir -p "$dir"

san="DNS:localhost,IP:127.0.0.1"
old_ifs=$IFS; IFS=','
for h in $CERT_HOSTNAMES; do
  h=$(echo "$h" | tr -d ' ')
  [ -z "$h" ] && continue
  if echo "$h" | grep -Eq '^[0-9.]+$|:'; then san="$san,IP:$h"; else san="$san,DNS:$h"; fi
done
IFS=$old_ifs

echo "2110 DAW: creating self-signed certificate for $san"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "$dir/key.pem" -out "$dir/cert.pem" \
  -subj "/CN=2110 DAW" -addext "subjectAltName=$san" >/dev/null 2>&1
