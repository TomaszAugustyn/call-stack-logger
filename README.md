# Call Stack Logger #

Call Stack Logger uses function instrumentation to facilitate logging of
every function call. Each nesting adds an ident, whereas returning from a
function removes it. As the result call stack tree is produced at the runtime
giving knowledge of the actual program's flow of execution.
## :seedling: Outcome ##

![Call Stack logger capture](misc/call-stack-logger-capture.gif)
## :book: Article ##

Here is the article on dev.to describing the details of the project, its aim and motivation
behind it: \
[Call Stack Logger - Function instrumentation as a way to trace program’s flow of execution](https://dev.to/taugustyn/call-stack-logger-function-instrumentation-as-a-way-to-trace-programs-flow-of-execution-419a)

## :scroll: Requirements ##

### GNU Binutils ###

It is required in order to get access to BFD (Binary File Descriptor
library) necessary to get information about object files and manipulate them.

```bash
sudo apt-get install binutils-dev
```

## :wrench: Building and running ##

```bash
git clone https://github.com/TomaszAugustyn/call-stack-logger.git
cd call-stack-logger

# Create build folder and go there
mkdir build && cd build

# Configure cmake with default logging
cmake ..
# or for extended logging you can play with these flags
cmake -DLOG_ADDR=ON -DLOG_NOT_DEMANGLED=ON ..
# or to compile your application with disabled instrumentation (no logging)
cmake -DDISABLE_INSTRUMENTATION=ON ..

# Build
make

# Build and Run (as the result trace.out file will be generated)
make run
```

## :wrench: Building and running - legacy (Makefiles) ##

```bash
git clone https://github.com/TomaszAugustyn/call-stack-logger.git
cd call-stack-logger

mv Makefile_legacy Makefile
mv src/Makefile_legacy src/Makefile

# Build with default logging
make
# or for extended logging you can play with these flags
make log_with_addr=1 log_not_demangled=1
# or to compile your application with disabled instrumentation (no logging)
make disable_instrumentation=1

# Build and Run (as the result trace.out file will be generated)
make run
```

## :balance_scale: Copyright and License ##

Call Stack Logger is a single-copyright project: all the source code in this [Call Stack Logger
repository](https://github.com/TomaszAugustyn/call-stack-logger) is Copyright &copy; Tomasz Augustyn.

As copyright owner, I dual license Call Stack Logger under different license terms, and
offers the following licenses for Call Stack Logger:
- GNU AGPLv3, a popular open-source license with strong
[copyleft](https://en.wikipedia.org/wiki/Copyleft) conditions (the default license)
- Commercial or closed-source licenses

If you license Call Stack Logger under AGPLv3, there is no license fee or signed license
agreement: you just need to comply with the AGPLv3 terms and conditions. See
[LICENSE_COMMERCIAL](./LICENSE_COMMERCIAL) and [LICENSE](./LICENSE) for further information.

If you purchase a commercial or closed-source license for Call Stack Logger, you must comply
with the terms and conditions listed in the associated license agreement; the
AGPLv3 terms and conditions do not apply. To purchase commercial license please contact me via email at t.augustyn@poczta.fm in order to discuss requirements and formulate a commercial license that best suits your needs.

The Call Stack Logger software itself remains the same: the only difference between an open-source Call Stack Logger and a commercial Call Stack Logger are the license terms.
