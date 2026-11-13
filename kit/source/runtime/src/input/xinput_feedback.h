#ifndef XBOX_XINPUT_FEEDBACK_H
#define XBOX_XINPUT_FEEDBACK_H
#include <windows.h>
#include <xinput.h>

typedef DWORD (WINAPI *XinputFeedbackSend)(DWORD,XINPUT_VIBRATION *);
typedef unsigned (*XinputFeedbackOutputPolicy)(void);
typedef void (*XinputFeedbackTrace)(const char *,DWORD,XINPUT_VIBRATION,XINPUT_VIBRATION,DWORD,int,unsigned);
typedef struct {
    SRWLOCK lock;
    int focused;
    unsigned active[4];
    /* Read-only port mask, queried under lock; NULL allows every port. */
    XinputFeedbackOutputPolicy output_policy;
    XinputFeedbackTrace trace;
} XinputFeedback;

/* Serialize output against focus and menu/source changes. Query the current
 * policy inside the lock so callbacks arriving out of order cannot restore
 * a stale permission. Regaining permission never replays an old effect. */
static unsigned xinput_feedback_allowed(XinputFeedback *state)
{
    return state->focused?(state->output_policy?state->output_policy():15u):0;
}
static void xinput_feedback_stop_disallowed(XinputFeedback *state,XinputFeedbackSend send,const char *reason)
{
    unsigned allowed=xinput_feedback_allowed(state);
    XINPUT_VIBRATION stopped={0};
    for(DWORD port=0;port<4;port++)if(!(allowed&(1u<<port)) && state->active[port]) {
        DWORD result=send(port,&stopped);
        if(state->trace)state->trace(reason,port,stopped,stopped,result,state->focused,allowed);
        state->active[port]=0;
    }
}
static DWORD xinput_feedback_send(XinputFeedback *state,DWORD port,
    XINPUT_VIBRATION value,XinputFeedbackSend send)
{
    if(port>=4)return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&state->lock);
    XINPUT_VIBRATION requested=value;
    unsigned allowed=xinput_feedback_allowed(state);
    if(!(allowed&(1u<<port)))
        value.wLeftMotorSpeed=value.wRightMotorSpeed=0;
    DWORD result=send(port,&value);
    state->active[port]=result==ERROR_SUCCESS &&
        (value.wLeftMotorSpeed || value.wRightMotorSpeed);
    if(state->trace)state->trace("guest_output",port,requested,value,result,state->focused,allowed);
    ReleaseSRWLockExclusive(&state->lock);
    return result;
}

static void xinput_feedback_focus(XinputFeedback *state,int focused,XinputFeedbackSend send)
{
    AcquireSRWLockExclusive(&state->lock);
    state->focused=focused!=0;
    xinput_feedback_stop_disallowed(state,send,"focus_stop");
    ReleaseSRWLockExclusive(&state->lock);
}
static void xinput_feedback_policy_changed(XinputFeedback *state,XinputFeedbackSend send)
{
    AcquireSRWLockExclusive(&state->lock);
    xinput_feedback_stop_disallowed(state,send,"policy_stop");
    ReleaseSRWLockExclusive(&state->lock);
}
#endif
