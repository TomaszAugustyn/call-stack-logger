# Call Stack Logger #

Call Stack Logger uses function instrumentation to facilitate logging of
every function call. Each nesting adds an ident, whereas returning from a
function removes it. As the result call stack tree is produced at the runtime
giving knowledge of the actual program's flow of execution.

![Call Stack logger capture](https://user-images.githubusercontent.com/9121868/113233386-66f94680-929f-11eb-9448-48ad56f215a6.mp4)

## Requirements ##

### GNU Binutils ###

It is required in order to get access to BFD (Binary File Descriptor
library) necessary to get information about object files and manipulate them.

```bash
sudo apt-get install binutils-dev
```

## Building and running ##
