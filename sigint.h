#ifndef SIGINT_H
#define SIGINT_H

#ifndef WINDOWS
void sig_catch(int sig_no);
static void sig_catch2(int sig_no);
#endif

#endif
