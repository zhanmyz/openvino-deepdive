
# Build
```
bash -x setup.sh
```

# Run
```
# Set Environment variables
export OVPATH=openvino/bin/intel64/Debug && \
export PYTHONPATH=$OVPATH/python:$OVPATH:$PYTHONPATH && \
export LD_LIBRARY_PATH=$OVPATH:$LD_LIBRARY_PATH

python -c "import openvino as ov; print(ov.__version__)"

# Convert model
python model_conversion/convert_model_yolo.py

# Run sample
./bin/object_detection_yolo models/yolo/yolo26n_openvino_model/yolo26n.xml data/images/person/person_detection.png GPU
```

---

# Source walkthrough (object_detection_yolo.cpp)

This document is aimed at newcomers to C++ / OpenCV / OpenVINO. It explains, line by line, what every function argument means, what it does, and **why it is done that way**.

---

## 1. Headers

```cpp
#include <chrono>
```
Pulls in the **time-measurement library**. Used to take a timestamp before and after inference; the difference gives the inference latency in milliseconds.
**Why we need it:** inference performance is a key metric and must be quantified.

```cpp
#include <fstream>
```
Pulls in the **file I/O library**, which provides `std::ifstream` (input file stream).
Used to read `metadata.yaml` line by line and parse YOLO class names.
**Why we need it:** class names are not hard-coded; they are loaded dynamically from the metadata file shipped with the model, so swapping models requires no code change.

```cpp
#include <iomanip>
```
Pulls in the **output formatting library**, which provides `std::fixed` (fixed-point format) and `std::setprecision(N)` (number of decimals).
Example: `3.14159265` formatted with `std::fixed << std::setprecision(4)` becomes `3.1416`.
**Why we need it:** confidences and inference times in the JSON output need a fixed number of decimals; otherwise scientific notation (e.g. `3.14e+00`) would break the JSON syntax.

```cpp
#include <iostream>
```
Pulls in the **standard I/O library**.
- `std::cout` -- print normal messages to the terminal (stdout)
- `std::cerr` -- print error messages to the terminal (stderr)
**Why distinguish cout/cerr:** the two go to different streams, so a script can redirect only errors via `2>error.log` without mixing them with regular output.

```cpp
#include <map>
```
Pulls in the **ordered map container** (red-black tree implementation). Used to store the `{ class id -> class name }` mapping.
Example: `names[0] = "person"`, `names[1] = "bicycle"`.
**Why a map instead of a vector:** YOLO class ids may not be contiguous; a map is safer for lookup by key, and it sorts entries by id automatically, which helps debugging.

```cpp
#include <sstream>
```
Pulls in the **string stream** library. `std::ostringstream` behaves like a virtual file -- you can append content with `<<` and retrieve the full string in one shot with `.str()`.
**Why not concatenate with `+`:** repeated `+` on strings creates temporary objects every time, which is slow; `ostringstream` uses an internal buffer and is much more efficient.

```cpp
#include <string>
#include <vector>
```
- `std::string` -- dynamic-length string type
- `std::vector<T>` -- dynamic array; you can `push_back` at any time and memory is managed automatically.
**Why a vector instead of a C array:** the size of a C array must be known at compile time, but the number of detection boxes is only known at runtime, so a dynamic container is required.

```cpp
#include <opencv2/dnn.hpp>
```
Pulls in OpenCV's **deep-learning module**. We only use `cv::dnn::NMSBoxes` (the non-maximum suppression function).
**Why we need NMS:** the NMS thresholds baked into a YOLO model may be quite permissive (especially for FP16 quantised models), leaving many overlapping boxes. A second pass of NMS in C++ acts as a fine-grained filter.

```cpp
#include <opencv2/imgcodecs.hpp>
```
Pulls in OpenCV's **image encode/decode module**, which provides:
- `cv::imread(path)` -- read an image from disk (jpg/png/bmp/...).
- `cv::imwrite(path, image)` -- write the processed image back to disk.

```cpp
#include <opencv2/imgproc.hpp>
```
Pulls in OpenCV's **image-processing module**, used here for:
- `cv::resize` -- resize an image
- `cv::cvtColor` -- colour-space conversion (BGR->RGB)
- `cv::split` -- split a multi-channel image into individual single-channel images
- `cv::rectangle` -- draw a rectangle on an image

```cpp
#include "openvino/openvino.hpp"
```
Pulls in the **OpenVINO inference engine** API (`ov::Core`, `ov::Model`, `ov::Tensor`, ...).
**Why OpenVINO instead of using PyTorch directly:** OpenVINO is Intel's inference framework optimised for Intel hardware (CPU/GPU/NPU); the same model usually runs several times faster on OpenVINO than on the original training framework.

---

## 2. `load_class_names`

Reads the `metadata.yaml` file shipped with a YOLO model and parses the class-id-to-name mapping.

**Example metadata.yaml format:**
```yaml
names:
  0: person
  1: bicycle
  2: car
```

```cpp
static std::map<int, std::string> load_class_names(const std::string& yaml_path) {
```
- `static` -- restricts the function to this translation unit (this .cpp file), avoiding name clashes with same-named functions in other files.
- `const std::string&` -- `const` means the function will not modify the string; `&` is pass-by-reference, avoiding the cost of copying the whole string.
- Return type `std::map<int, std::string>` -- key is the int class id, value is the string class name.

```cpp
    bool in_names = false;
```
**State flag.** A YAML file may contain several top-level keys (`nc:`, `task:`, `names:`, ...); this flag remembers "am I currently inside the names section?" so other sections are not mis-parsed as class names.

```cpp
    while (std::getline(f, line)) {
```
`std::getline(stream, str)` reads one full line (up to `\n`) into `line`. When it returns false, end-of-file is reached and the loop exits.

```cpp
        if (!line.empty() && line[0] != ' ' && line[0] != '\t')
            break;
```
The moment a non-empty line that does not start with a space or tab appears, the `names:` section has ended (YAML sub-items must be indented). `break` exits the loop to avoid consuming unrelated content.

```cpp
        key_str.erase(0, key_str.find_first_not_of(" \t"));
```
Strip leading spaces and tabs ("left trim", ltrim).
Example: `"  0"` -> first non-whitespace is at position 2 -> `erase(0, 2)` -> `"0"`.

```cpp
        if (!val.empty() && val.back() == '\r')
            val.pop_back();
```
Remove the Windows carriage-return `\r`. Windows line endings are `\r\n`; Linux uses `\n`. When reading a Windows-edited YAML file on Linux, `getline` only consumes `\n` and leaves the trailing `\r`; without removing it, class names become `"person\r"` and matching fails.

---

## 3. JSON helpers

```cpp
static std::string json_str(const std::string& s) {
    return "\"" + s + "\"";
}
```
Wraps a string with double quotes to make it valid JSON.
Example: `json_str("person")` -> `"\"person\""` -> prints as `"person"`.
**Why roll our own instead of using a JSON library:** the source comment says "Minimal JSON helpers" -- the JSON the program emits has a fixed, simple shape, so manual concatenation is enough. Pulling in nlohmann/json would only add a build dependency.

```cpp
    gmtime_r(&t, &tm_utc);
```
Converts a Unix timestamp to a UTC time struct.
- The trailing `r` in `gmtime_r` stands for "reentrant", i.e. the thread-safe variant; `&t` is the input, `&tm_utc` is the output.
- Unlike the older `gmtime`, `gmtime_r` writes into a caller-provided buffer instead of a global static, eliminating races in multi-threaded scenarios.

---

## 4. `main`

### Argument parsing

```cpp
    if (argc != 4) {
```
- `argc` (argument count) -- number of command-line tokens, including the program name itself. E.g. `./object_detection_yolo model.xml image.png GPU` has 4 tokens, so `argc = 4`.
- If it is not 4 the user passed the wrong number of arguments: print usage and exit.

```cpp
        constexpr float CONF_THRESHOLD    = 0.5f;
        constexpr float NMS_IOU_THRESHOLD = 0.45f;
        constexpr int   INPUT_SIZE        = 640;
```
- `constexpr` -- compile-time constants; the compiler substitutes the literal value at every usage site, so there is zero runtime overhead.
- `CONF_THRESHOLD = 0.5f` -- **confidence threshold**: detections below 50% are considered false positives and discarded. Higher reduces false positives; lower detects more (but potentially noisier) objects.
- `NMS_IOU_THRESHOLD = 0.45f` -- **IoU (Intersection over Union) threshold**: the overlap area divided by the union area; if the ratio exceeds 45%, the two boxes are considered duplicates of the same object and only the one with the higher confidence is kept.
- `INPUT_SIZE = 640` -- the fixed 640x640 input size of the YOLO model. The value comes from the training configuration and cannot be changed casually (changing it requires re-training or re-exporting the model).

### Steps 1-2: initialise OpenVINO

```cpp
        ov::Core core;
```
Creates the **OpenVINO runtime core**. It discovers available inference devices (CPU/GPU/NPU), reads/compiles models, and creates inference requests. A program only ever needs one `Core` instance.

```cpp
        std::shared_ptr<ov::Model> model = core.read_model(model_path);
```
- `core.read_model()` -- reads the `.xml` model topology file (OpenVINO IR format). An OpenVINO IR consists of two files: `.xml` (network structure) and `.bin` (weights) with the same stem. `read_model` automatically finds the matching `.bin` in the same directory.
- `std::shared_ptr` -- a **shared smart pointer** with an internal reference count; memory is released automatically when the last owner is destroyed. Unlike raw pointers, you never write `delete`, avoiding leaks.

```cpp
        if (model->inputs().size() != 1) {
            std::cerr << "[ ERROR ] Sample supports only single input topologies"
```
**Why we check the number of inputs:** the program logic assumes a single-image input. Some models have multiple inputs (e.g. joint detection + segmentation); this program does not handle them and the early check produces a clear error message.

### Step 3: image preprocessing

Preprocessing is the most critical stage of the inference pipeline. **Preprocessing must exactly match what was done at training time**, otherwise the input distribution differs and inference results will be heavily biased or completely wrong.

#### Beginner section: what does "preprocessing must exactly match training" really mean?

"Preprocessing" refers to every mathematical transformation applied to an image before it is fed into the neural network (resizing, channel reordering, value scaling, ...).

A simple analogy: **the model is like a person who only eats potatoes that have been cut into one-inch cubes, peeled, and boiled**.
- During training, every potato fed to him was "cubed, peeled, boiled" -> he learned to recognise potatoes in that form.
- At inference time, if you throw him a whole muddy raw potato, he will not recognise it at all -- not because he is stupid, but because the "input form" differs from what he was trained on.

Neural networks are the same. Every training image went through the same preprocessing pipeline (e.g. resize to 640x640, RGB order, divide by 255). Every weight in the model was tuned for that "form" of input. At inference time, a different preprocessing pipeline produces a completely different distribution of numbers.

**Concrete example: a red pixel goes wrong if preprocessing is inconsistent**

Assume the standard training preprocessing is `BGR->RGB` followed by `/255`. A red pixel in the original image is `B=30, G=30, R=220`.

| Scenario | Channel order | /255? | Model input (read as R,G,B) | How the model interprets it |
|----------|---------------|-------|-----------------------------|------------------------------|
| (OK) Training-standard | RGB | yes | `R=0.863, G=0.118, B=0.118` | "strong red, weak blue/green" -> red object |
| (BUG) Forgot BGR->RGB | BGR | yes | `R=0.118, G=0.118, B=0.863` | "strong blue" -> model thinks it is a blue object (a stop sign might be classified as sky) |
| (BUG) Forgot /255 | RGB | no | `R=220, G=30, B=30` | values 255x larger than training -> activations saturate -> almost all noise |
| (BUG) No resize to 640x640 | RGB | yes | wrong shape | OpenVINO immediately throws a shape mismatch |

**Real-world consequences:**
A photo clearly containing 5 people, run with wrong preprocessing, might produce:
- 0 detections (all confidences below the 0.5 threshold),
- or random classes like "sports ball" or "toaster",
- or boxes nowhere near any object.

**Most common preprocessing mistakes for beginners:**
1. Forgetting BGR->RGB (OpenCV defaults to BGR; PyTorch trains with RGB).
2. Forgetting /255 (pixels still in 0-255 integer range).
3. Forgetting HWC->NCHW dimension reordering.
4. Different resize method (training may use letterboxing while inference uses a plain stretching resize -- YOLO has some robustness to this, but accuracy still drops).
5. Skipping mean/std normalisation (some models require subtracting mean and dividing by std after /255; this YOLO model does not).

**Remember:** *whatever the training pipeline does to the image, the inference pipeline must reproduce exactly.*

---

```cpp
        cv::Mat image = cv::imread(image_path);
```
- `cv::Mat` -- OpenCV's core data structure, fundamentally a multi-dimensional numeric matrix. It can represent images (2D matrix whose elements are pixel values), feature maps, etc.
- `cv::imread(path)` -- read an image from disk. **By default returns BGR (blue-green-red) channel order**, `uint8` (each channel an integer in 0..255).
- **Why BGR rather than RGB?** A historical OpenCV quirk: the early underlying image libraries used BGR order, and OpenCV kept it for backward compatibility.

#### Beginner section: how can `image.dims == 2` store 3-channel BGR?

This is one of the most confusing points for beginners. **Key idea: `cv::Mat::dims` describes the "geometric dimensions" (rows, cols) only; it does not include the channel "dimension". Channels live inside each matrix cell and are not counted as a separate dimension.**

#### First, the most fundamental question: is putting 3 numbers per cell mathematically legal?

It is. The "each cell is one number" picture you learned at school is only the **most common case: a scalar matrix**. Mathematically and in programming, it is perfectly fine to have "each cell is a vector". There are several names for this; they all mean the same thing:

| Way of looking at it | Form |
|----------------------|------|
| (1) Each cell holds a length-3 vector `[B,G,R]` | shape `4x4`, element type is "3-vector" |
| (2) Split into 3 same-sized scalar matrices stacked | shape `3x4x4` (3 grayscale 4x4 images) |
| (3) A 3D array / order-3 tensor | shape `4x4x3` (H x W x C) |

**These three views are mathematically equivalent**, only the data layout differs. OpenCV picked view (1): geometry stays 2D (so `image.at(y, x)` row/column indexing feels natural), and the 3 channels are packed into the element type `cv::Vec3b`. That is why `dims=2` while `channels()=3` -- no contradiction.

> Analogy: an Excel sheet is still a "two-dimensional table" even if each cell contains a triple "name + phone + address". The sheet is still 2D (rows x cols); having 3 fields per cell does not make it 3D.

> In strict mathematical language, this object is a "4x4 matrix valued in R^3", or a "4x4x3 third-order tensor". The "one number per cell" you learned in school is the special case (valued in R^1).

#### Want to see a real example?

A runnable demo lives in this directory: [cv_mat_demo/](cv_mat_demo/)

It contains:
- [generate_demo.py](cv_mat_demo/generate_demo.py) -- builds a 4x4 colour image by hand with numpy, exports it as BMP, and dumps every matrix / attribute / memory layout to a text file.
- [color_4x4.bmp](cv_mat_demo/color_4x4.bmp) -- the real 4x4-pixel colour image (tiny, almost invisible).
- [color_4x4_x40.bmp](cv_mat_demo/color_4x4_x40.bmp) -- the same image scaled 40x for visual inspection of the 16 colour blocks.
- [matrix_dump.txt](cv_mat_demo/matrix_dump.txt) -- the attribute table, the 2D matrix (each cell `[B,G,R]`), the 3 grayscale matrices obtained by splitting the channels, and how the 48 bytes are arranged in memory.

**To run:**
```bash
cd samples/cpp/cv_mat_demo
python generate_demo.py
```

Open `matrix_dump.txt` and you will immediately see "a 4x4 colour image is a 4-row by 4-column matrix where each cell holds 3 integers in 0..255". View `color_4x4_x40.bmp` to see the corresponding colours, and the "matrix <-> colour image" correspondence will click.

#### Beginner section: why does a single-channel matrix show as "grayscale"? How does colour appear?

This is a very fundamental question that touches the whole "numbers <-> colour" chain. Two layers below.

##### Layer 1: how does a screen actually display the colour of a pixel?

Put a magnifying glass on a bright white area of your phone/monitor: each screen pixel is actually made of **three tiny lamps packed close together: a red lamp, a green lamp, and a blue lamp**. These tiny lamps are called **sub-pixels**.

```
  One screen pixel                 Magnified -- sub-pixel structure
  +------------+                  +--+--+--+
  |            |   ->   zoom in   |R |G |B |     each of R/G/B has its own brightness (0-255)
  |            |                  |  |  |  |
  +------------+                  +--+--+--+
```

| Term | Meaning |
|------|---------|
| **Pixel** | The smallest "colour unit" on the screen; emits light through 3 sub-pixels. |
| **Sub-pixel** | Independent red / green / blue lamp inside a pixel, each with its own brightness. |
| **Resolution** | How many rows and columns of pixels the screen has, e.g. `1920x1080`. |
| **Bit depth / colour depth** | How many bits encode the brightness of each sub-pixel. Common: 8 bits = 0-255 = 256 levels, so 1 pixel = 3 x 8 = 24 bits ~ 16.7M colours. |
| **Additive primaries** | Mixing red, green, and blue light at various intensities can synthesise nearly every colour the eye can see. |

**So the full chain for "screen displays colour" is:**

```
3 numbers in the matrix (B, G, R)
        |
        v
GPU translates them into "brightness levels for the red/green/blue sub-pixels"
        |
        v
The red lamp, green lamp, and blue lamp at the corresponding screen position emit at those brightnesses
        |
        v
The three light beams mix in your eye -> the brain perceives a colour
```

Example (one pixel BGR=`[30, 30, 220]`):
- Blue lamp brightness ~ 30/255 = 12% -> nearly off.
- Green lamp brightness ~ 30/255 = 12% -> nearly off.
- Red lamp brightness ~ 220/255 = 86% -> very bright.
The three lights mix -> you see a darkish red.

> Red + green + blue can synthesise any colour because the human retina happens to have three types of cone cells sensitive to different wavelengths (long / medium / short, colloquially R/G/B). Screens use three light beams to stimulate those three receptors, "fooling" the brain into perceiving any colour. This is dictated by biology, not by an arbitrary screen choice.

##### Layer 2: why does a single-channel matrix "display as grey"?

Back to the question: take any **single-channel matrix** of B, G, or R (`4x4x1`, one number per cell) and display it -- the result is always grey (black-and-white). The reason:

**Key point: a single-channel matrix gives the screen only "brightness", with no "colour information".**

When an image viewer sees an `H x W x 1` matrix, it cannot tell whether the number means "red" or "blue". It can only treat the number as a brightness level and ask the red, green, and blue sub-pixels to **all light up to that brightness simultaneously**:

| Value in matrix | Red lamp | Green lamp | Blue lamp | Mixed light -> what you see |
|-----------------|----------|------------|-----------|------------------------------|
| 0 | 0 | 0 | 0 | full black |
| 128 | 128 | 128 | 128 | medium grey |
| 255 | 255 | 255 | 255 | brightest white |
| 200 | 200 | 200 | 200 | bright grey |

**Red light + green light + blue light mixed at equal intensity is physically white light (or grey at low intensity)**. So single-channel images live on the black-grey-white scale and never show colour -- it is not that computers "default to grey", it is that "R=G=B at equal brightness" must produce grey.

##### Side-by-side example: 6 images already generated in this directory

In [cv_mat_demo/](cv_mat_demo/):

| File | Data shape | Meaning | What you see on screen |
|------|------------|---------|------------------------|
| `color_4x4_x40.bmp` | 4x4x**3** | full colour image | 16 colour blocks (red/green/blue/white/...) |
| `channel_B_gray.bmp` | 4x4x**1** (B only) | single-channel matrix | **grey** (larger B = whiter) |
| `channel_G_gray.bmp` | 4x4x**1** (G only) | single-channel matrix | **grey** (larger G = whiter) |
| `channel_R_gray.bmp` | 4x4x**1** (R only) | single-channel matrix | **grey** (larger R = whiter) |
| `channel_B_color.bmp` | 4x4x3 (B kept, G/R=0) | 3-channel, only blue lamp lit | **pure blue** (other lamps off) |
| `channel_G_color.bmp` | 4x4x3 (G kept, B/R=0) | 3-channel, only green lamp lit | **pure green** |
| `channel_R_color.bmp` | 4x4x3 (R kept, B/G=0) | 3-channel, only red lamp lit | **pure red** |

Compare `channel_B_gray.bmp` and `channel_B_color.bmp`: **their B-channel values are identical**, but the former is `H x W x 1` (1 channel) while the latter is `H x W x 3` (with the other two channels filled with 0). Same numbers, different instructions to the screen:
- 1 channel -> the screen treats it as brightness; R, G, B lamps all light to that value -> grey.
- 3 channels (only B non-zero) -> only the blue lamp lights; red and green are off -> pure blue.

**This directly proves the root cause of "single-channel must be grey, three-channel can be colour": it depends on how many lamps the numbers ultimately drive.**

##### One-liner summary

> - **Colour = the red/green/blue lamps emit at their own brightness and mix to one colour** (a consequence of monitor hardware plus human-eye biology).
> - **A 3-channel matrix** gives the screen 3 independent brightness numbers -> 3 lamps light independently -> colour.
> - **A 1-channel matrix** gives the screen only 1 brightness number -> all 3 lamps share it -> R=G=B -> physically must be grey.
> - Resolution is irrelevant: resolution only tells how many pixels (how many rows/columns of lamp triples) the screen has, not how brightly each lamp shines.

---

**What each `cv::Mat` attribute means for a 1920x1080 colour image:**

| Attribute | Value | Meaning |
|-----------|-------|---------|
| `image.dims` | `2` | number of geometric dimensions: height, width -> 2D |
| `image.rows` | `1080` | height (number of rows) |
| `image.cols` | `1920` | width (number of columns) |
| `image.channels()` | `3` | numbers per pixel (per matrix cell) |
| `image.type()` | `CV_8UC3` | 8-bit unsigned integer x 3 channels |
| `image.elemSize()` | `3` | bytes per cell (B + G + R, 1 byte each) |

**A picture: what a 4x4 BGR colour image looks like inside `cv::Mat`**

It is still a 4-row by 4-column 2D matrix (`dims=2`), but **each cell holds a length-3 array `[B, G, R]` rather than a single number**:

```
          col 0           col 1           col 2           col 3
      +--------------+--------------+--------------+--------------+
row 0 | [B,G,R]      | [B,G,R]      | [B,G,R]      | [B,G,R]      |
      +--------------+--------------+--------------+--------------+
row 1 | [B,G,R]      | [B,G,R]      | [B,G,R]      | [B,G,R]      |
      +--------------+--------------+--------------+--------------+
row 2 | [B,G,R]      | [B,G,R]      | [B,G,R]      | [B,G,R]      |
      +--------------+--------------+--------------+--------------+
row 3 | [B,G,R]      | [B,G,R]      | [B,G,R]      | [B,G,R]      |
      +--------------+--------------+--------------+--------------+
        ^
  Geometrically a 4x4 2D matrix (dims=2), but each cell embeds 3 values -> channels()=3
```

**Concrete numbers: a 2x2 image (top-left red, top-right green, bottom-left blue, bottom-right white)**

```
          col 0              col 1
      +--------------+--------------+
row 0 | [ 30, 30,220]| [ 30,200, 30]|   <- red, green
      +--------------+--------------+
row 1 | [220, 30, 30]| [255,255,255]|   <- blue, white
      +--------------+--------------+
```
Attributes: `dims=2, rows=2, cols=2, channels()=3`.

**How it is actually laid out in memory (a flat byte stream, HWC layout):**

```
address: 0   1   2    3   4   5    6   7   8    9  10  11
value: [ 30, 30,220][ 30,200, 30][220, 30, 30][255,255,255]
        |-row0col0-| |-row0col1-| |-row1col0-| |-row1col1-|
         red pixel    green pixel  blue pixel   white pixel
```
The B, G, R bytes of each pixel are stored consecutively, then the next pixel follows. This is the well-known **HWC layout (Height x Width x Channel, channels innermost)**.

**How to read a particular pixel's particular channel in C++:**

```cpp
// Get the B, G, R values of the pixel at row 100, column 200
cv::Vec3b pixel = image.at<cv::Vec3b>(100, 200);  // Vec3b = length-3 uchar array
uchar B = pixel[0];   // blue
uchar G = pixel[1];   // green
uchar R = pixel[2];   // red
```
Notice that the template parameter `Vec3b` of `at<cv::Vec3b>` already expresses the idea "each cell contains 3 bytes" -- channel information is encoded in the element type, not as an additional matrix dimension.

**Contrast: when does `dims` actually exceed 2?**

`cv::Mat` does support genuine multi-dimensional tensors (e.g. a neural-net feature map):

```cpp
int sizes[4] = {1, 3, 640, 640};
cv::Mat tensor(4, sizes, CV_32F);   // a true 4D tensor with dims=4
// tensor.dims == 4, channels() == 1
```
Here each cell holds a single float, but the matrix itself has 4 geometric dimensions.

**One-liner summary:**
> For an ordinary image `cv::Mat`: **geometric dimensions = 2 (rows x cols)**, **the BGR 3 channels live inside each cell and are not counted as an independent dimension**. So `dims=2` and `channels=3` are not in conflict.

```cpp
        cv::Mat resized;
        cv::resize(image, resized, cv::Size(INPUT_SIZE, INPUT_SIZE));
```
**`cv::resize` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (src) | `image` | input image (original resolution) |
| 2 (dst) | `resized` | output image (where the result is stored) |
| 3 (dsize) | `cv::Size(640, 640)` | target size, `Size(width, height)` |

**Why resize to 640x640:** the convolution weights of the YOLO model were trained for 640x640 input. The resize introduces stretching (the original aspect ratio is not preserved), but YOLO is robust enough to tolerate it.
**Why not reshape the model:** as the code comment notes, this YOLO model contains Transformer attention layers whose shapes are fixed and cannot be reshaped dynamically through the OpenVINO reshape API.

---

#### Critical step: BGR -> RGB conversion

```cpp
        cv::Mat rgb;
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
```

**`cv::cvtColor` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (src) | `resized` | input image, BGR channel order |
| 2 (dst) | `rgb` | output image, conversion result |
| 3 (code) | `cv::COLOR_BGR2RGB` | conversion type: BGR -> RGB |

**Why this conversion is mandatory:**
The YOLO model was trained with PyTorch; PyTorch's image-reading libraries (PIL, ...) default to RGB order. Every training image the model saw was in RGB, and its weights "learned" that the R channel carries red information and the B channel carries blue.
OpenCV reads in BGR. Without conversion, the model would treat blue as red, dropping accuracy badly.

**Example:** a red stop sign (R=220, G=30, B=30):
- BGR-stored order: `[30, 30, 220]` (blue first).
- Without conversion the model sees "red channel" = 30 (actually blue) and "blue channel" = 220 (actually red).
- The model mistakes the red sign for a blue object -> wrong classification.
- After conversion to RGB-stored order `[220, 30, 30]`, the model gets the red information correctly.

---

#### Critical step: data-type normalisation

```cpp
        rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);
```

**`cv::Mat::convertTo` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (m, output) | `rgb` | output target -- here the same matrix (in-place conversion) |
| 2 (rtype) | `CV_32F` | target type: 32-bit single-precision float (C++ `float`), range +/- 3.4e38 |
| 3 (alpha, scale) | `1.0 / 255.0 ~ 0.00392` | each pixel value is multiplied by this |
| 4 (beta, offset) | (omitted, default 0) | added to every value after scaling |

**Full conversion formula: `output = input * alpha + beta`.**
Here: `output = pixel * (1/255) + 0`.

**Why both steps are needed:**

1. **Type conversion (uint8 -> float32):** network weights are floats, so the input must also be float to take part in matrix multiplications. `uint8` cannot be used directly.

2. **Normalisation (/255, scale 0..255 -> 0.0..1.0):** matches training. YOLO trained with `img / 255.0`, so inference must do the same. Without normalisation the input range is 255x larger than during training, activations saturate, and the network is unusable.

**Example:**
```
Raw pixel (uint8):  R=200,  G=100,  B=50
Normalised (float): R=0.784, G=0.392, B=0.196
```
After normalisation everything is in 0.0..1.0, matching the training distribution.

---

#### Critical step: HWC -> NCHW conversion

```cpp
        std::vector<cv::Mat> ch(3);
        cv::split(rgb, ch);
```

**`cv::split` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (src) | `rgb` | input: 3-channel image (H x W x 3) |
| 2 (mv) | `ch` | output: array of 3 single-channel images |

After this call: `ch[0]` = R channel (640x640 matrix), `ch[1]` = G channel, `ch[2]` = B channel.

**Why split is needed:**
In memory the image is laid out as **HWC (Height x Width x Channel)**, i.e. the RGB values of each pixel are stored consecutively:
```
pixel(0,0):R G B | pixel(0,1):R G B | pixel(1,0):R G B | ...
memory: [R,G,B, R,G,B, R,G,B, ...]
```
Neural networks (PyTorch / OpenVINO) expect input in **NCHW (Batch x Channel x Height x Width)** layout, i.e. all pixels of one channel stored contiguously:
```
all R: [R,R,R,...] | all G: [G,G,G,...] | all B: [B,B,B,...]
memory: [R,R,R,...(640x640 of them), G,G,G,..., B,B,B,...]
```
`cv::split` is the first step from HWC to per-channel separation.

```cpp
        std::vector<float> input_data(1 * 3 * INPUT_SIZE * INPUT_SIZE);
```
Allocates a contiguous block of memory of size `1(batch) * 3(channels) * 640 * 640 = 1,228,800` floats (~4.7 MB).
**Why contiguous memory:** `ov::Tensor` requires the input to be a single contiguous block (C-style array). `std::vector` guarantees its underlying storage is contiguous.

```cpp
        for (int c = 0; c < 3; ++c) {
            const float* src = reinterpret_cast<const float*>(ch[c].data);
            std::copy(src, src + INPUT_SIZE * INPUT_SIZE,
                      input_data.data() + c * INPUT_SIZE * INPUT_SIZE);
        }
```
- `ch[c].data` -- raw byte pointer of channel `c`, type `unsigned char*`.
- `reinterpret_cast<const float*>` -- since `convertTo` already converted the data to float, force the compiler to read this memory as a float array.
- `std::copy(begin, end, dst)` -- copy `640 * 640 = 409600` floats to the specified offset of the destination.
  - `c=0` (R): goes to `input_data[0]` ... `input_data[409599]`.
  - `c=1` (G): goes to `input_data[409600]` ... `input_data[819199]`.
  - `c=2` (B): goes to `input_data[819200]` ... `input_data[1228799]`.

After the loop, `input_data` is exactly in NCHW layout and can be handed to OpenVINO directly.

---

### Steps 4-5: compile the model and run synchronous inference

```cpp
        ov::CompiledModel compiled_model = core.compile_model(model, device_name);
```
- `core.compile_model(model, device)` -- **compile** the generic IR model into an executable form for the target device:
  - `"CPU"` -- vectorised with AVX2/AVX-512.
  - `"GPU"` -- compiled to OpenCL kernels for Intel integrated/discrete GPUs.
- **Why "read model" and "compile model" are separate steps:** the result of read is a device-agnostic intermediate representation; compilation is the hardware-specific optimisation step (which can take several seconds). Keeping them separate lets you compile one model for multiple devices, or create multiple inference requests from one compiled model (parallel inference).

```cpp
        ov::InferRequest infer_request = compiled_model.create_infer_request();
```
Create an **inference request**. A `CompiledModel` can create many `InferRequest`s; each has its own input/output buffers and can run in parallel. This program only needs one.

```cpp
        ov::Tensor input_tensor(
            ov::element::f32,
            {1, 3, static_cast<size_t>(INPUT_SIZE), static_cast<size_t>(INPUT_SIZE)},
            input_data.data());
```

**`ov::Tensor` constructor parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (element_type) | `ov::element::f32` | element data type: 32-bit float |
| 2 (shape) | `{1, 3, 640, 640}` | tensor shape NCHW |
| 3 (host_ptr) | `input_data.data()` | pointer to the actual data (**no copy, zero-copy**) |

`static_cast<size_t>(INPUT_SIZE)` -- `ov::Shape` elements are `size_t` (unsigned), while `INPUT_SIZE` is `int` (signed); the explicit cast silences a compiler warning.
**Why zero-copy:** copying 1.2 MB again would waste memory and time. `ov::Tensor` references the storage of `input_data` directly; `input_data` must not be released while inference runs.

```cpp
        auto t0 = std::chrono::high_resolution_clock::now();
        infer_request.infer();
        auto t1 = std::chrono::high_resolution_clock::now();
        const double inference_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
```
- `high_resolution_clock` -- the highest-resolution clock available, usually nanoseconds.
- `infer_request.infer()` -- **synchronous inference**: blocks the calling thread until inference finishes.
- `std::chrono::duration<double, std::milli>` -- convert the time difference to a `double` value in milliseconds.
  - `double` = numeric type.
  - `std::milli` = unit ratio = 1/1000 (millisecond = 1/1000 second).

---

### Step 6: process inference output

```cpp
        const ov::Tensor output_tensor = infer_request.get_output_tensor();
        const ov::Shape  out_shape     = output_tensor.get_shape();  // [1, 300, 6]
        const size_t     num_boxes     = out_shape[1];   // 300
        const size_t     box_size      = out_shape[2];   // 6
        const float*     raw           = output_tensor.data<const float>();
```
The output tensor of YOLO (ultralytics export format) has shape `[1, 300, 6]`:
- `1` -- batch size.
- `300` -- the model keeps at most 300 detections (after its built-in NMS).
- `6` -- 6 floats per box: `[x1, y1, x2, y2, confidence, class_id]`. Coordinates are in 640x640 pixel space.

```cpp
        const float scale_x = static_cast<float>(orig_w) / INPUT_SIZE;  // e.g. 1920/640 = 3.0
        const float scale_y = static_cast<float>(orig_h) / INPUT_SIZE;  // e.g. 1080/640 = 1.6875
```
**Why scale factors are needed:** at inference time the image was scaled to 640x640, so the output coordinates are in that 640x640 space. To draw boxes on the **original resolution** image we must multiply by the scale ratio.

```cpp
            const float* det = raw + i * box_size;
```
Offset the flat `raw` pointer to the start of the i-th box.
Example: for the third box (`i=2`), `raw + 2*6 = raw + 12`; reading `det[0]..det[5]` gives the 6 numbers of that box.

```cpp
            cv::Rect bbox(xmin, ymin, xmax - xmin, ymax - ymin);
```
**`cv::Rect` constructor parameters (note: not two corners but top-left + width/height!):**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (x) | `xmin` | top-left x |
| 2 (y) | `ymin` | top-left y |
| 3 (width) | `xmax - xmin` | width (not the right-bottom x) |
| 4 (height) | `ymax - ymin` | height (not the right-bottom y) |

```cpp
            candidates.push_back({cid, label, score, bbox});
            boxes_vec.push_back(bbox);
            scores_vec.push_back(score);
```
Two parallel arrays `boxes_vec` and `scores_vec` are maintained because `cv::dnn::NMSBoxes` wants the boxes and scores as separate lists (not as an array of structs).

---

### Second NMS pass

```cpp
        cv::dnn::NMSBoxes(boxes_vec, scores_vec,
                          CONF_THRESHOLD, NMS_IOU_THRESHOLD, keep_indices);
```

**`cv::dnn::NMSBoxes` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (bboxes) | `boxes_vec` | all candidate boxes (`cv::Rect` array) |
| 2 (scores) | `scores_vec` | confidence score for each box |
| 3 (score_threshold) | `0.5f` | boxes below this score are dropped before NMS |
| 4 (nms_threshold) | `0.45f` | boxes whose IoU exceeds this are duplicates; keep the highest-scoring one |
| 5 (indices, output) | `keep_indices` | indices of the kept boxes (output) |

**How NMS works -- example:**
Three "person" detections with confidences 0.92, 0.88, 0.75 all overlap each other with IoU > 0.45:
1. Sort by score: 0.92 > 0.88 > 0.75.
2. Keep the 0.92 box.
3. Compute IoU between the rest and the 0.92 box: 0.88 -> IoU=0.80 > 0.45 -> drop; 0.75 -> IoU=0.60 > 0.45 -> drop.
4. Only the 0.92 box remains.

---

### Drawing and output

```cpp
            cv::rectangle(image, c.bbox, cv::Scalar(0, 255, 0), 2);
```

**`cv::rectangle` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (img) | `image` | image to draw on (modified in place) |
| 2 (rec) | `c.bbox` | rectangle to draw (`cv::Rect`) |
| 3 (color) | `cv::Scalar(0, 255, 0)` | colour, `Scalar(B, G, R)` = pure green |
| 4 (thickness) | `2` | line thickness in pixels; `-1` means fill |

Note that `cv::Scalar` uses BGR order (consistent with OpenCV's image storage): `Scalar(blue, green, red)`. Use `Scalar(0, 0, 255)` for a red box.

```cpp
        cv::imwrite("out.bmp", image);
```

**`cv::imwrite` parameters:**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| 1 (filename) | `"out.bmp"` | output file name; format is decided by the extension |
| 2 (img) | `image` | image matrix to write to disk |

**Why `.bmp` instead of `.jpg`:** BMP is lossless, with no compression artefacts -- handy when debugging, you can see the box edges exactly. For production, `.png` (lossless compression) or `.jpg` (lossy compression) give smaller files.

---

### Exception handling

```cpp
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
```
- `std::exception` -- the base class of every exception in the C++ standard library; OpenVINO and OpenCV both throw exceptions that derive from it, so a single `catch` block catches them all.
- `ex.what()` -- returns a C-string describing the error, e.g. `"[GPU] Failed to compile model: unsupported layer type"`.
- `return EXIT_FAILURE` -- non-zero exit code (value 1); the OS and calling scripts can detect failure by inspecting the exit code.

---

## 5. End-to-end data flow

```
Disk image (arbitrary resolution, BGR uint8)
    | cv::imread
cv::Mat image (HxWx3, BGR, uint8)
    | cv::resize
cv::Mat resized (640x640x3, BGR, uint8)
    | cv::cvtColor (BGR->RGB, align channel order with training)
cv::Mat rgb (640x640x3, RGB, uint8)
    | convertTo (uint8->float32, /255 normalise to [0,1])
cv::Mat rgb (640x640x3, RGB, float32, [0.0~1.0])
    | cv::split + manual copy (HWC->NCHW)
std::vector<float> input_data (1x3x640x640, NCHW)
    | ov::Tensor + infer_request.infer()
Output [1, 300, 6] (x1,y1,x2,y2,score,class_id)
    | confidence filter + coordinate rescale + NMS
Final detection boxes (in original image coordinates)
    | cv::rectangle + cv::imwrite
out.bmp (image annotated with boxes)
    + JSON printed to the terminal
```
