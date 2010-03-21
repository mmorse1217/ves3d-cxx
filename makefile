DOX = doxygen

.PHONY: docs clean

docs: 
	$(DOX) ./docs/Doxyfile

clean: 
	-rm -rf *.o ./src/*.o  ./docs/latex ./docs/html 

include makefiles/makefile.in
include makefiles/makefile.in.common

