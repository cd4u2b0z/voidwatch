# vhs tape — sandbox mode demo
# regenerate with: vhs docs/sandbox.tape

Output docs/sandbox.gif

Set Shell "zsh"
Set FontSize 14
Set Width 1280
Set Height 720
Set Padding 20
Set TypingSpeed 80ms

# Run the sandbox: N-body sim, parallax stars, Perlin nebula, particles.
# 14 seconds of ambient orbital motion is enough to see the trails breathe.
Type "voidwatch --no-audio"
Enter
Sleep 14s

# Toggle the help overlay briefly so viewers see there's a key reference.
Type "?"
Sleep 2s
Type "?"
Sleep 1s

# Quit cleanly.
Type "q"
Sleep 500ms
