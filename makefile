CC = gcc
CFLAGS = -Wall

all: overseer controller
	echo "Done"

overseer: overseer.c
	$(CC) $(CFLAGS) overseer.c -o overseer -pthread

controller: controller.c
	$(CC) $(CFLAGS) controller.c -o controller

clean:
	rm -f controller
	rm -f overseer
	rm -f *.o

.PHONY: all clean