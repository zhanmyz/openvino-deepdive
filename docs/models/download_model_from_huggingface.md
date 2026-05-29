
# Set Proxy
```
export http_proxy=http://<proxy>:<port>
export https_proxy=http://<proxy>:<port>
```

# Download Model
NOTE: The original HuggingFace link of model file has 'blob' and can't download directly by `wget` command, need to replace the `blob` with `resolve`, for example: `https://.../blob/main/...  --> https://.../resolve/main/`
```
wget -O yolo26n.pt https://huggingface.co/Ultralytics/YOLO26/resolve/main/yolo26n.pt
curl -L -o yolo26n.pt https://huggingface.co/Ultralytics/YOLO26/resolve/main/yolo26n.pt
```
 * -L: Follow redirection
 * -o: output file
