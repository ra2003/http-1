
let command = Cmd.locate("testHttp") + " -v --iterations 2 " + test.mapVerbosity(-1)
let result = Cmd.run(command)
print("Running\n" + result)
printf("   [Command] ")

