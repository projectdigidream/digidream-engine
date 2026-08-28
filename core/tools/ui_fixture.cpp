// A UI_OPEN control message on stdout, framed exactly as the server frames one.
//
// The client's decoder is a SECOND implementation of this wire format, written
// in a different language, and the only thing keeping the two in step is that
// they were written from the same spec. That is not a mechanism. This gives the
// JS test real bytes from the C++ encoder to decode, so a field added on one
// side and forgotten on the other fails a test instead of a session.
//
// Built and run by client/test/ui.test.js.
#include <cstdio>
#include "dnp/proto/messages.hpp"
using namespace dnp;
using namespace dnp::proto;
static void t(UiText& d, const char* s){ u16 n=0; while(s[n]&&n<UI_MAX_TEXT){d.text[n]=s[n];++n;} d.len=n; }
int main(){
    UiPanel p; p.ui_id=9; t(p.title,"Debug panel"); p.count=4;
    p.widgets[0].kind=UiWidgetKind::Label;  p.widgets[0].id=1; t(p.widgets[0].label,"Player");
    p.widgets[1].kind=UiWidgetKind::Slider; p.widgets[1].id=2; t(p.widgets[1].label,"Speed");
    p.widgets[1].value=8; p.widgets[1].lo=1; p.widgets[1].hi=40;
    p.widgets[2].kind=UiWidgetKind::Toggle; p.widgets[2].id=3; t(p.widgets[2].label,"Noclip");
    p.widgets[2].value=1;
    p.widgets[3].kind=UiWidgetKind::Select; p.widgets[3].id=4; t(p.widgets[3].label,"Map");
    p.widgets[3].option_count=3;
    t(p.widgets[3].options[0],"MAP01"); t(p.widgets[3].options[1],"MAP02"); t(p.widgets[3].options[2],"MAP03");
    u8 buf[8192]; ByteWriter w(buf,sizeof(buf));
    const size_t lp=w.mark(); w.u32v(0); w.u8v(MSG_UI_OPEN); encode_ui_open(w,p);
    const size_t end=w.size(); const u32 body=(u32)(end-lp-4);
    buf[lp]=(u8)(body&0xFF); buf[lp+1]=(u8)((body>>8)&0xFF);
    buf[lp+2]=(u8)((body>>16)&0xFF); buf[lp+3]=(u8)((body>>24)&0xFF);
    fwrite(buf,1,end,stdout);
    return 0;
}
