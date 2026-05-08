# vhs tape — astro mode demo
# regenerate with: vhs docs/astro.tape

Output docs/astro.gif

Set Shell "zsh"
Set FontSize 13
Set Width 1440
Set Height 900
Set Padding 20
Set TypingSpeed 80ms

# Astro mode. Location resolves from ~/.config/voidwatch/location.conf
# so the demo doesn't bake any specific lat/lon into the recording.
Type "voidwatch --astro --no-audio"
Enter
Sleep 6s

# Bump time-scrub once to 10× — visible motion without dizzy strobing.
Type "+"
Sleep 7s

# Reveal the constellation lines, sit with them.
Type "l"
Sleep 6s

# Switch to heliocentric perspective. With orbital traces drawn, the
# top-down solar system reads cleanly at 10× speed.
Type "m"
Sleep 9s

# Back to geocentric, slow time, end on a steady frame.
Type "m"
Sleep 1s
Type "0"
Sleep 4s

Type "q"
Sleep 500ms
