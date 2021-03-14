CC      := gcc
CCFLAGS := -Ofast
LDFLAGS :=

TARGETS:= decode
MAINS  := $(addsuffix .o, $(TARGETS) )
OBJ    := v4l2.o drm.o util.o $(MAINS)
DEPS   := 

.PHONY: all clean

all: $(TARGETS)

clean:
	rm -f $(TARGETS) $(OBJ)

$(OBJ): %.o : %.c $(DEPS)
	$(CC) -c -o $@ $< $(CCFLAGS)

$(TARGETS): % : $(filter-out $(MAINS), $(OBJ)) %.o
	$(CC) -o $@ $(LIBS) $^ $(CCFLAGS) $(LDFLAGS)