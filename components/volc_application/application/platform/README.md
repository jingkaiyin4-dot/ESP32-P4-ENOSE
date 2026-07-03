# Platform

## brief introduction

This directory serves as the software abstraction layer for the Hardware Conversational AI Agent Development Kit, built around the business characteristic of AI conversation. Its purpose is to shield hardware details, enabling users to focus on software development and business implementation.
Among this directory:

- inc: Contains header files
- src: Contains specific implementations for different hardware platforms

## principles

As a Hardware Abstraction Layer (HAL), we typically expect capabilities to be atomized, interfaces to be orthogonal, and high flexibility to be maintained. However, excessive pursuit of atomization and flexibility often reduces interface usability and impairs business development efficiency. These two aspects are often mutually exclusive. // Nevertheless, this development platform focuses on AI conversation scenarios; therefore, given the relatively fixed business scenarios, we can make trade-offs with greater engineering practical value. // Our core design principles are as follows:

1. Orthogonal interfaces to reduce coupling and shield hardware details.
2. Interface design takes certain business characteristics into account:
   some implementations and configuration parameters are implicitly implemented according to business requirements,and not all software configurable items are retained.This prevents upper-layer business development from being bogged down in endless detailed configuration and parameter tuning. For example: we enable the Acoustic Echo Cancellation (AEC) module by default for audio capture,instead of requiring upper-layer business to configure aec_enabled in the config file;thus, many interface designs at this layer do not make complete sense.For detailed explanations and trade-offs, refer to the implementation details.Interface design takes certain business characteristics into account: ails.

## implementation

### volc_hal

volc_hal_init() Do the necessary actions after the hardware is powered on, it is not necessary to initialize all the hardware modules, but it is necessary to set the device name.

volc_get_global_hal_context() ithis function could help you get the hal context everywhere. Do not Modify the return value

### volc_hal_player

We do not specify specific media configurations (e.g., encoding format, sampling rate, or resolution).This is because we expect all the aforementioned parameters to be uniquely determined and immutable across all scenarios.Therefore, these values must be fixed at compile time; if your device supports multiple parameters,macros should be used to select the desired ones at compile time.This design adheres to Principle 2.

for volc_hal_player_play_data()，We have not specified whether this function is sync or async. However, it is strongly recommended to implement this function as non-blocking with a certain internal buffer, ensuring that the caller of the function does not need to overly consider the function's call frequency.  For example, the function can be called every 1 second to play 1.2 seconds of audio or video data.

### volc_hal_capture

We do not specify specific media configurations (e.g., encoding format, sampling rate, or resolution).This is because we expect all the aforementioned parameters to be uniquely determined and immutable across all scenarios.Therefore, these values must be fixed at compile time; if your device supports multiple parameters,macros should be used to select the desired ones at compile time.This design adheres to Principle 2.

To ensure a uniform rate for media capture data, we adopt the callback function approach: frame-by-frame audio and video data is delivered asynchronously via callbacks, instead of providing an active capture interface. Therefore, during the function implementation, a capture thread needs to be created to asynchronously callback the data out.

We consider both voice wake-up and voice data capture to fall under the category of audio capture, differing only in mode (internal pipeline). For this reason, we do not redesign a separate module for voice wake-up; instead, we add an additional mode specifically for voice wake-up.

Finally, note that acoustic echo cancellation (AEC) must be implemented for audio capture.

### volc_hal_display

Typically, a set of screen display interfaces allows upper-layer callers to independently create and destroy regions for display purposes.However, to minimize the development effort of upper-layer business logic,we do not allow the business side to create and manage different screen regions on their own.Instead, different screen regions are divided and created during the execution of volc_hal_display_create().The business layer can only obtain different render layer objects,pass data to them, and perform rendering operations. For example, we divide the screen into a status bar(VOLC_DISPLAY_OBJ_STATUS), main screen(VOLC_DISPLAY_OBJ_MAIN), and subtitle display bar(VOLC_DISPLAY_OBJ_SUBTITLE).You may define your own divisions, as all these codes are open-source and modifiable.

### volc_hal_file

File systems may have different implementations on different devices,but we generally adhere to the ANSI C style.For devices without a file system, these interfaces may be left unimplemented as this is not mandatory.
