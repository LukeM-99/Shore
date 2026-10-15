CFLAGS += -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags webkit2gtk-4.1)
LDLIBS += $(shell pkg-config --libs webkit2gtk-4.1)

shore: src/*.c src/*.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

clean:
	rm -f shore

.PHONY: clean
