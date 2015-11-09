#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

uint32_t G_HANDLE = 1;

struct server_node {
	int total;
	pthread_key_t handle_key;
};

static struct server_node G_NODE;

void 
server_globalinit(void) {
	G_NODE.total = 0;
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
}

uint32_t 
server_current_handle(void) {
	void * handle = pthread_getspecific(G_NODE.handle_key);
	return (uint32_t)(uintptr_t)handle;
}

static void *
_malloc(void* p) {
	uint32_t handle = G_HANDLE++;
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(handle));
	__RUNTIME("pthread create: 0x%x", handle);
	int i;
	for (i = 0; i < 5; ++i) {
		server_malloc(1024*(i+1));
	}
}

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

int
main(int argc, char const *argv[]) {
	server_globalinit();
	
	pthread_t pids[4];
	int i;
	for (i=0; i<4; i++) {
		create_thread(&pids[i], _malloc, "_malloc");
	}

	for (i=0; i<4; i++) {
		pthread_join(pids[i], NULL); 
	}
	
	dump_c_mem();
	return 0;
}