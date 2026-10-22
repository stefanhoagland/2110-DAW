#!/bin/sh
# Start the AES67 receiver next to nginx, and start it again if it ever stops.
# nginx passes /aes67/ to it (see nginx.conf).
(
  while true; do
    node /opt/aes67/server/aes67-server.mjs
    echo "AES67 receiver stopped; starting it again"
    sleep 2
  done
) &
