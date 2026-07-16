export mainfolder=$(realpath .)
mkdir $mainfolder/example_compiled
gcc -Wall -Wextra -I"$mainfolder" example/simple.c src/http_server.c -o example_compiled/simple -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/filerouter.c src/http_server.c -o example_compiled/filerouter -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/simple_https.c src/http_server.c -o example_compiled/simple_https -lssl -lcrypto -lpthread
gcc -Wall -Wextra -I"$mainfolder" example/filerouter_https.c src/http_server.c -o example_compiled/filerouter_https -lssl -lcrypto -lpthread
