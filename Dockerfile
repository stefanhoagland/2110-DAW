# 2110 DAW: static web app served by nginx over HTTP and HTTPS.
# Browsers only allow audio-interface access on HTTPS (or localhost), so the
# container creates a self-signed certificate on first start if none is provided.
FROM nginx:1.27-alpine

RUN apk add --no-cache openssl

COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
COPY docker/10-make-cert.sh /docker-entrypoint.d/10-make-cert.sh
RUN chmod +x /docker-entrypoint.d/10-make-cert.sh

COPY index.html /usr/share/nginx/html/
COPY css /usr/share/nginx/html/css
COPY js /usr/share/nginx/html/js

# Certificates live here; mount it to keep the same certificate across updates
# or to drop in your own cert.pem and key.pem.
VOLUME /config
ENV CERT_HOSTNAMES=""

EXPOSE 80 443
