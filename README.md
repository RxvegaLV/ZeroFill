# ZeroFill
A windows based python tool used to wipe USB flashdrives past recovery using zero fill
This uses a simple method of filling the USB flashdrive with a huge file that is meant to take up all space, then the user manually deletes, or reformats

HOW TO USE:
(note that before use, you reformat, or delete everything from the flashdrive) You simply run the .exe (or .py if you prefer) and it should open a
window where you select the USB flashdrive that you want to clear, and then press "WRITE ZEROES" It could take some time, so be patient.

⚠IMPORTANT NOTE⚠
the .exe and the .py file use different fill methods, the .exe file (coded with C++) might have faster write speeds than the .py file.
