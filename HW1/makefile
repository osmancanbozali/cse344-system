all: clear clean compile

compile: main.c
	@echo "-------------------------------------------"
	@echo "Compiling..."
	@gcc -o fileManager main.c
	@echo "-------------------------------------------"
	@echo "Run Example: ./fileManager <command> [arguments]"


clean:
	@echo "-------------------------------------------"
	@echo "Cleaning..."
	@rm -f *.o
	@rm -f fileManager
	@rm -f *.txt
	@find . -type d -not -path "." -not -path "./.git*" -exec rm -rf {} +

clear:
	@clear
