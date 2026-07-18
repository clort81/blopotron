echo "to compile (debug): gcc -g -O0 -Wall blopotron46.c -o blopotron -lm -lSDL2"
echo "to compile normmal: gcc    -O2 -Wall blopotron46.c -o blopotron -lm -lSDL2"
echo "Controls are move: wasd  fire: ijkl"
echo "To run with text-rendering run "
echo "in a fairly big terminal window"
echo ""
echo "HARDFRAME=1 ./blopotron -t"
echo ""
echo "The hardframe envvar drops the fps from 60 to 30"
echo ""
echo "Then Switch to sdl window and hit '1' to insert coin."
echo "Keyboard input goes to  sdl window, observe output in terminal window."
echo ""
echo "Ctrl-C to break out or return to try launching the game"
read $char
HARDFRAME=1 ./blopotron -t

