# Compilatore e flags
CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lpthread

# File sorgenti
SERVER_SRC = server.c map.c
CLIENT_SRC = client.c map.c

# File oggetto
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Nomi eseguibili
SERVER_EXE = server
CLIENT_EXE = client

# Target di default
all: $(SERVER_EXE) $(CLIENT_EXE)

# Compila il server
$(SERVER_EXE): $(SERVER_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Compila il client
$(CLIENT_EXE): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Regola per compilare i .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pulizia
clean:
	rm -f $(SERVER_EXE) $(CLIENT_EXE) *.o

.PHONY: all clean
