#include <cstdio>
#include <iostream>

int main() {
	int bytes = 0;
	
	while(fgetc(stdin) != EOF) {
		bytes += 1;

	}
	fprintf(stdout, "%i %s", bytes, "\n");
}
