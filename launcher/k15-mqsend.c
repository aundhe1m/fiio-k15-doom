// SPDX-License-Identifier: GPL-2.0-or-later
/* k15-mqsend: send one ASCII message to a FiiO K15 POSIX message queue.
 * Usage: k15-mqsend [/player] <msg>
 * Protocol (mq_ui -> mq_player on "/player"):
 *   "<cmd:4 hex><total_len:4 hex><ext:4 hex>[payload]"
 *   e.g. set input to Local Play: 0657000C0008
 */
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *q = "/player", *msg;
    if (argc == 3) {
        q = argv[1];
        msg = argv[2];
    } else if (argc == 2) {
        msg = argv[1];
    } else {
        fprintf(stderr, "usage: %s [queue] <msg>\n", argv[0]);
        return 2;
    }
    mqd_t mq = mq_open(q, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        return 1;
    }
    if (mq_send(mq, msg, strlen(msg), 0) != 0) {
        perror("mq_send");
        return 1;
    }
    mq_close(mq);
    return 0;
}
