#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_FRAME_TIMES_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_FRAME_TIMES_H_

namespace Libs::Graphics::FrameTimes {

// Frame pacing for tools (--frame-times). The presentation thread calls Presented() after each
// successful present; the window thread calls Report() once a second, which prints every
// interval since the previous report as one line: "FrameTimes: 16.7 16.6 33.4 @4.2" (milliseconds,
// oldest first; "@" = time since the newest present, absent before the first frame; no intervals
// when nothing was presented that second). No-ops without the option.
void Presented();
void Report();

} // namespace Libs::Graphics::FrameTimes

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_FRAME_TIMES_H_ */
