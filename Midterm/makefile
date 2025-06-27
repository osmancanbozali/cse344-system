CC=gcc
CFLAGS= -g -Wno-format-truncation
LDFLAGS= -pthread -lrt

all: BankServer BankClient

BankServer: server.c
	@$(CC) $(CFLAGS) server.c -o BankServer $(LDFLAGS)
	@echo "BankServer compiled successfully"
	@echo "Usage: ./BankServer <bankName> <ServerFIFO>"

BankClient: client.c
	@$(CC) $(CFLAGS) client.c -o BankClient $(LDFLAGS)
	@echo "BankClient compiled successfully"
	@echo "Usage: ./BankClient <ClientFile> <ServerFIFO>"

clean:
	@rm -f BankServer BankClient *.o *.bankLog
	@echo "Cleaned build artifacts."
