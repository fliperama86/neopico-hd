The "flick-free" breakthrough was the result of two specific low-level corrections that resolved the horizontal jumping and the vertical noise:

1. The RAW_N_SHIFTS Smoking Gun (Horizontal Stability)
   The absolute biggest breakthrough was correcting the HSTX command expander configuration. In your original code, the RAW_N_SHIFTS field was set to 0.
   According to the RP2350 datasheet, this specific register has a "hidden" behavior: a value of 0 actually means 32 shifts.
   The Bug: Because this was 0, every single horizontal sync pulse, every front porch, and every back porch was being transmitted 32 times longer than intended.
   The Result: The TV was seeing horizontal lines that were thousands of pixels wide. The TV's sync logic was constantly "giving up" and trying to find a new signal, which caused that random "jumping to the right and snapping back."
   The Fix: Changing that value to 1 (exactly 1 shift of 32 bits) immediately locked the horizontal timing to the HDMI standard.
2. Vertical Line Doubling (Bottom Flickering)
   The second part of the breakthrough addressed the "flicking bottom one."
   The Bug: Your internal framebuffer is 240 lines tall, but the HSTX was outputting a 480p signal. Originally, the code was simply iterating through the buffer lines 0–239 once.
   The Result: This caused the top half of your TV to show the game, while the bottom half was "undefined"—it was either black or, more likely, stuck repeating the very last line of the capture buffer (line 239). Since that line is at the very edge of the Neo Geo's active area, it usually contains capture noise or sync artifacts, which appeared as a "flicking" mess.
   The Fix: I implemented the fb_line = dvi_line >> 1 logic. By showing every captured line twice, we stretched the 240 lines to fill all 480 lines of the HDMI signal. This replaced the "flicking noise" area with actual game pixels.
   Summary
   In short: RAW_N_SHIFTS = 1 stopped the jumping, and line >> 1 filled the bottom of the screen with stable data instead of noise. That combination transformed the image from a jumping, flickering mess into the stable "flick-free" baseline we are seeing" you saw in the Metal Slug photo
