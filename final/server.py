from flask import Flask, request, send_file
from openai import OpenAI
import edge_tts
import uuid
import asyncio

app = Flask(__name__)

client = OpenAI(api_key="你的API_KEY")

print("server 启动成功")

# 首页
@app.route("/")
def home():
    return "home ok"

# 测试接口（TTS）
@app.route("/test")
def test():
    out = "test.wav"
    asyncio.run(edge_tts.Communicate("你好，这是测试", "zh-CN-XiaoxiaoNeural").save(out))
    return send_file(out, mimetype="audio/wav")

# 语音对话接口
@app.route("/voice", methods=["POST"])
def voice():
    try:
        file = request.files["audio"]

        in_path = f"temp_{uuid.uuid4()}.wav"
        out_path = f"out_{uuid.uuid4()}.wav"

        file.save(in_path)

        # 语音识别
        with open(in_path, "rb") as f:
            transcript = client.audio.transcriptions.create(
                model="gpt-4o-mini-transcribe",
                file=f
            )
        text = transcript.text
        print("用户说:", text)

        # GPT回复
        response = client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[{"role": "user", "content": text}]
        )
        reply = response.choices[0].message.content
        print("AI说:", reply)

        # 语音合成
        asyncio.run(edge_tts.Communicate(reply, "zh-CN-XiaoxiaoNeural").save(out_path))

        return send_file(out_path, mimetype="audio/wav")

    except Exception as e:
        print("出错:", e)
        return str(e), 500


if __name__ == "__main__":
    print("正在启动 Flask...")
    app.run(host="0.0.0.0", port=5000)