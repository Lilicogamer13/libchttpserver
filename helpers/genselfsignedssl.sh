openssl req -x509 -newkey rsa:4096 -keyout example/key.pem -out example/cert.pem -sha256 -days 365 -nodes -subj "/CN=localhost"
