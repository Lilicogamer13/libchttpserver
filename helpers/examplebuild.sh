export mainfolder=$(realpath .)
mkdir $mainfolder/example_compiled
openssl req -x509 -newkey rsa:4096 -keyout $mainfolder/example/key.pem -out $mainfolder/example/cert.pem -sha256 -days 365 -nodes -subj "/CN=localhost"
gcc -Wall -Wextra -I"$mainfolder" example/simple.c src/http_server.c -o example_compiled/simple -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/filerouter.c src/http_server.c -o example_compiled/filerouter -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/simple_https.c src/http_server.c -o example_compiled/simple_https -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/filerouter_https.c src/http_server.c -o example_compiled/filerouter_https -lssl -lcrypto -lpthread
