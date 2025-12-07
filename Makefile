PROG=	p6psgmmlc
SRCS=	main.c mml_compiler.c
OBJS=	${SRCS:.c=.o}

CFLAGS+=	-Wall
#CFLAGS+=	-DDEBUG

${PROG}:	${OBJS}
	${CC} -o ${PROG} ${CFLAGS} ${LDFLAGS} ${OBJS} ${LDLIBS}

${OBJS}: mml_compiler.h

.PHONY: test

TESTDIR=	testdata
test:	${PROG}
	./${PROG} ${TESTDIR}/test-ok.mml test-ok.bin
	-./${PROG} ${TESTDIR}/test-error.mml test-error.bin

CLEANFILES+=	*.bin

clean:
	-rm -f ${PROG} *.o *.core
	-rm -f ${CLEANFILES}

