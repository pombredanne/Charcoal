# _very_ incomplete

#INCLUDE_DIRS = -I. -I../ThirdParty/OpenPA/Releases/1.0.4/Install/include
INCLUDE_DIRS = -I. -I /usr/local/stow/openpa-1.0.4/include -pg

all:
	gcc -g -Wall -c -o ../Build/semaphore.o $(INCLUDE_DIRS) semaphore.c
	gcc -g -Wall -c -o ../Build/charcoal_runtime.o $(INCLUDE_DIRS) charcoal_runtime.c
	gcc -g -Wall -c -o ../Build/charcoal_std_lib.o charcoal_std_lib.c
	mkdir -p ../Install/lib
	ar rcs ../Install/lib/libcharcoal_sys.a ../Build/semaphore.o ../Build/charcoal_runtime.o ../Build/charcoal_std_lib.o
	mkdir -p ../Install/include
	cp charcoal_runtime.h ../Install/include
	cp charcoal_std_lib.h ../Install/include
