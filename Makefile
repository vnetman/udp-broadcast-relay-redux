udp-broadcast-relay-redux: main.c
	gcc -O3 -Wall -Wno-trigraphs main.c -o udp-broadcast-relay-redux

clean:
	rm -f udp-broadcast-relay-redux
