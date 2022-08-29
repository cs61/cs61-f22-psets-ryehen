#include <cstdio>
#include <cctype>

int main() {
	unsigned long lines = 0;
	unsigned long words = 0;
	unsigned long bytes = 0;

	char c;
	bool nextCouldBeWord = false;
	
	while((c = char(fgetc(stdin))) != EOF) {
		if(c == '\n') {
			lines ++;
		}

		if(isspace(c) == false) {
			nextCouldBeWord = true;
		}

		if(isspace(c) == true && nextCouldBeWord == true) {
			words++;
			nextCouldBeWord = false;
		}

		bytes++;
	}
	
	if(nextCouldBeWord == true) {
		words++;
	}

	fprintf(stdout, "%lu %lu %lu\n", lines, words, bytes);
}
