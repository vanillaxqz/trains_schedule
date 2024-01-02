all:
	gcc client.c -o client
	g++ server.cpp -lpthread -o server
clean:
	rm -f client server