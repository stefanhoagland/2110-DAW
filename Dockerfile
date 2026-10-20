# 2110 DAW: static web app served by nginx over HTTP and HTTPS.
# Browsers only allow audio-interface access on HTTPS (or localhost), so the
# container creates a self-signed certificate on first start if none is provided.

# Free third-party effect plugins (Web Audio Modules) bundled into the image.
FROM --platform=$BUILDPLATFORM node:22-alpine AS plugins
COPY docker/bundle-plugins.sh /bundle-plugins.sh
RUN sh /bundle-plugins.sh /bundled-plugins

FROM nginx:1.27-alpine

RUN apk add --no-cache openssl

COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
COPY docker/10-make-cert.sh /docker-entrypoint.d/10-make-cert.sh
RUN chmod +x /docker-entrypoint.d/10-make-cert.sh

COPY index.html /usr/share/nginx/html/
COPY css /usr/share/nginx/html/css
COPY js /usr/share/nginx/html/js
COPY --from=plugins /bundled-plugins /usr/share/nginx/html/bundled-plugins

# Certificates live here; mount it to keep the same certificate across updates
# or to drop in your own cert.pem and key.pem.
VOLUME /config
ENV CERT_HOSTNAMES=""

EXPOSE 80 443
