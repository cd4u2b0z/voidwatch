# vhs tape — astro mode demo
# regenerate with: vhs docs/astro.tape

Output docs/astro.gif

Set Shell "zsh"
Set FontSize 13
Set Width 1440
Set Height 900
Set Padding 20
Set TypingSpeed 80ms

# Astro mode at Charleston, SC. Skip ALSA so the cap doesn't depend on audio.
Type "voidwatch --astro --no-audio --lat 32.78 --lon -79.93"
Enter
Sleep 4s

# Speed up time-scrub three steps so motion is visible (1× → 1000×).
Type "+"
Sleep 600ms
Type "+"
Sleep 600ms
Type "+"
Sleep 4s

# Reveal the constellation lines.
Type "l"
Sleep 3s

# Use the search prompt: type "halley" and press enter.
# / is bound to the in-program search.
Type "/halley"
Enter
Sleep 3s

# Switch to heliocentric perspective — top-down solar system.
Type "m"
Sleep 6s

# Reset speed and quit.
Type "0"
Sleep 1s
Type "q"
Sleep 500ms
