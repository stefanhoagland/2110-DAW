# Soundbending Live: web app served by nginx over HTTP and HTTPS, plus a small
# Node service that receives AES67 / ST 2110-30 network audio for it (/aes67/ws).
# Browsers only allow audio-interface access on HTTPS (or localhost), so the
# container creates a self-signed certificate on first start if none is provided.

# Free third-party effect plugins (Web Audio Modules) bundled into the image.
FROM --platform=$BUILDPLATFORM node:22-alpine AS plugins
COPY docker/bundle-plugins.sh /bundle-plugins.sh
RUN sh /bundle-plugins.sh /bundled-plugins

FROM nginx:1.27-alpine

RUN apk add --no-cache openssl nodejs

COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
COPY docker/10-make-cert.sh /docker-entrypoint.d/10-make-cert.sh
COPY docker/20-aes67.sh /docker-entrypoint.d/20-aes67.sh
RUN chmod +x /docker-entrypoint.d/10-make-cert.sh /docker-entrypoint.d/20-aes67.sh

# AES67 receiver. Browsers can't join multicast audio, so this joins the streams
# and passes them to the page. The container must be on the streams' network
# (a custom/macvlan network such as br0 on Unraid, or host networking).
COPY server /opt/aes67/server
COPY js/aes67-sdp.js /opt/aes67/js/aes67-sdp.js
RUN echo '{"type":"module"}' > /opt/aes67/package.json

COPY index.html favicon.svg /usr/share/nginx/html/
COPY css /usr/share/nginx/html/css
COPY js /usr/share/nginx/html/js
COPY --from=plugins /bundled-plugins /usr/share/nginx/html/bundled-plugins

# Certificates live here; mount it to keep the same certificate across updates
# or to drop in your own cert.pem and key.pem.
VOLUME /config
ENV CERT_HOSTNAMES=""

EXPOSE 80 443 9875/udp 5004/udp
