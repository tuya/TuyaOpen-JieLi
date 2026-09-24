#include "tkl_queue.h"

#include "jieli_tkl_os.h"
#include "tuya_error_code.h"

#include <stdlib.h>
#include <string.h>

OPERATE_RET tkl_queue_create_init(TKL_QUEUE_HANDLE *queue, int msgsize, int msgcount)
{
    if (queue == NULL || msgsize <= 0 || msgcount <= 0) {
        return OPRT_INVALID_PARM;
    }
    JIELI_TKL_QUEUE *jieli_queue = calloc(1, sizeof(*jieli_queue));
    if (jieli_queue == NULL || os_q_create(&jieli_queue->queue, (QS)msgcount) != 0) {
        free(jieli_queue);
        return OPRT_OS_ADAPTER_QUEUE_CREAT_FAILED;
    }
    jieli_queue->message_size = (uint32_t)msgsize;
    *queue = (TKL_QUEUE_HANDLE)jieli_queue;
    return OPRT_OK;
}

OPERATE_RET tkl_queue_post(const TKL_QUEUE_HANDLE queue, void *data, uint32_t timeout)
{
    if (queue == NULL || data == NULL) {
        return OPRT_INVALID_PARM;
    }
    JIELI_TKL_QUEUE *jieli_queue = (JIELI_TKL_QUEUE *)queue;
    void *copy = malloc(jieli_queue->message_size);
    int result;
    if (copy == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    memcpy(copy, data, jieli_queue->message_size);
    /* The OS queue item is a pointer-sized value. Pass the address of the
     * local pointer so the queue copies the allocated payload pointer. */
    result = os_q_post_to_back(&jieli_queue->queue, &copy,
                               timeout == 0xFFFFFFFFu ? -1 : (int)timeout);
    if (result != 0) {
        free(copy);
    }
    return result == 0 ? OPRT_OK : OPRT_OS_ADAPTER_QUEUE_SEND_FAIL;
}

OPERATE_RET tkl_queue_fetch(const TKL_QUEUE_HANDLE queue, void *msg, uint32_t timeout)
{
    if (queue == NULL || msg == NULL) {
        return OPRT_INVALID_PARM;
    }
    JIELI_TKL_QUEUE *jieli_queue = (JIELI_TKL_QUEUE *)queue;
    void *copy = NULL;
    int result = os_q_pend(&jieli_queue->queue, jieli_tkl_timeout_to_ticks(timeout), &copy);
    if (result != 0) {
        return result == OS_TIMEOUT ? OPRT_OS_ADAPTER_QUEUE_RECV_FAIL : OPRT_OS_ADAPTER_QUEUE_RECV_FAIL;
    }
    if (copy != NULL) {
        memcpy(msg, copy, jieli_queue->message_size);
        free(copy);
    }
    return OPRT_OK;
}

void tkl_queue_free(const TKL_QUEUE_HANDLE queue)
{
    if (queue != NULL) {
        JIELI_TKL_QUEUE *jieli_queue = (JIELI_TKL_QUEUE *)queue;
        void *copy = NULL;

        /* The queue owns a heap copy for each posted message. Drain pending
         * messages before deleting the OS queue so their allocations are not
         * leaked. Callers must stop queue users before freeing the handle. */
        while (os_q_accept(&jieli_queue->queue, &copy) == 0) {
            free(copy);
            copy = NULL;
        }
        (void)os_q_del(&jieli_queue->queue, OS_DEL_ALWAYS);
        free(jieli_queue);
    }
}
