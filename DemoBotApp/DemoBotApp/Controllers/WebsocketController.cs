namespace DemoBotApp.Controllers
{
    using System;
    using System.Collections.Generic;
    using System.Configuration;
    using System.IO;
    using System.Linq;
    using System.Net;
    using System.Net.Http;
    using System.Threading;
    using System.Threading.Tasks;
    using System.Web;
    using System.Web.Http;
    using DemoBotApp.WebSocket;
    using Microsoft.Bing.Speech;
    using Microsoft.Bot.Connector.DirectLine;
    using NAudio.Wave;
    using Microsoft.Azure; // Namespace for CloudConfigurationManager
    using Microsoft.WindowsAzure.Storage; // Namespace for CloudStorageAccount
    using Microsoft.WindowsAzure.Storage.Blob; // Namespace for Blob storage types
    using NAudio.Wave;
    using System.Text;
    using System.Net.Http.Headers;

    [RoutePrefix("chat")]
    public class WebsocketController : ApiController
    {
        private static readonly Uri ShortPhraseUrl = new Uri(Constants.ShortPhraseUrl);
        private static readonly Uri LongDictationUrl = new Uri(Constants.LongPhraseUrl);
        private static readonly Uri SpeechSynthesisUrl = new Uri(Constants.SpeechSynthesisUrl);
        private static readonly string CognitiveSubscriptionKey = ConfigurationManager.AppSettings["CognitiveSubscriptionKey"];

        private SpeechClient speechRocognitionClient;
        private SpeechSynthesisClient ttsClient;
        private string speechLocale = Constants.SpeechLocale;
        private readonly CancellationTokenSource cts = new CancellationTokenSource();

        private DirectLineClient directLineClient;
        private static readonly string DirectLineSecret = ConfigurationManager.AppSettings["DirectLineSecret"];
        private static readonly string BotId = ConfigurationManager.AppSettings["BotId"];
        private static readonly string FromUserId = "TestUser";

        private WebSocketHandler defaultHandler = new WebSocketHandler();
        private static Dictionary<string, WebSocketHandler> handlers = new Dictionary<string, WebSocketHandler>();

        public WebsocketController()
        {
            // Setup bot client
            this.directLineClient = new DirectLineClient(DirectLineSecret);

            // Setup speech recognition client
            Preferences speechPreference = new Preferences(speechLocale, ShortPhraseUrl, new CognitiveTokenProvider(CognitiveSubscriptionKey));
            this.speechRocognitionClient = new SpeechClient(speechPreference);
            

            // Setup speech synthesis client
            SynthesisOptions synthesisOption = new SynthesisOptions(SpeechSynthesisUrl, CognitiveSubscriptionKey);
            this.ttsClient = new SpeechSynthesisClient(synthesisOption);
        }

        [Route]
        [HttpGet]
        public async Task<HttpResponseMessage> Connect(string nickName)
        {
            if (string.IsNullOrEmpty(nickName))
            {
                throw new HttpResponseException(HttpStatusCode.BadRequest);
            }

            WebSocketHandler webSocketHandler = new WebSocketHandler();

            // Handle the case where client forgot to close connection last time
            if (handlers.ContainsKey(nickName))
            {
                WebSocketHandler origHandler = handlers[nickName];
                handlers.Remove(nickName);

                try
                {
                    await origHandler.Close();
                }
                catch
                {
                    // unexcepted error when trying to close the previous websocket
                }
            }

            handlers[nickName] = webSocketHandler;

            string conversationId = string.Empty;
            string watermark = null;

            webSocketHandler.OnOpened += ((sender, arg) =>
            {
                Conversation conversation = this.directLineClient.Conversations.StartConversation();
                conversationId = conversation.ConversationId;
            });

            webSocketHandler.OnTextMessageReceived += (async (sender, message) =>
            {
                // Do nothing with heartbeat message
                // Send text message to bot service for non-heartbeat message
                if (!string.Equals(message, "heartbeat", StringComparison.OrdinalIgnoreCase))
                {
                    await OnTextMessageReceived(webSocketHandler, message, conversationId, watermark);
                }
            });

            webSocketHandler.OnBinaryMessageReceived += (async (sender, bytes) =>
            {
                await OnBinaryMessageReceived(webSocketHandler, bytes, conversationId, watermark);
            });

            webSocketHandler.OnClosed += (sender, arg) =>
            {
                //webSocketHandler.SendMessage(nickName + " Disconnected!").Wait();
                handlers.Remove(nickName);
            };

            HttpContext.Current.AcceptWebSocketRequest(webSocketHandler);
            return Request.CreateResponse(HttpStatusCode.SwitchingProtocols);
        }

        private async Task OnTextMessageReceived(WebSocketHandler handler, string message, string conversationId, string watermark)
        {
            await BotClientHelper.SendBotMessageAsync(this.directLineClient, conversationId, FromUserId, message);
            BotMessage botResponse = await BotClientHelper.ReceiveBotMessagesAsync(this.directLineClient, conversationId, watermark);

            // Convert text to speech
            byte[] totalBytes;
            if (botResponse.Text.Contains("Music.Play"))
            {
                totalBytes = ((MemoryStream)SampleMusic.GetStream()).ToArray();
                handler.SendBinary(totalBytes).Wait();
            }
            else
            {
                totalBytes = await ttsClient.SynthesizeTextToBytesAsync(botResponse.Text, CancellationToken.None);

                WaveFormat target = new WaveFormat(8000, 16, 2);
                MemoryStream outStream = new MemoryStream();
                using (WaveFormatConversionStream conversionStream = new WaveFormatConversionStream(target, new WaveFileReader(new MemoryStream(totalBytes))))
                {
                    WaveFileWriter.WriteWavFileToStream(outStream, conversionStream);
                    outStream.Position = 0;
                }

                handler.SendBinary(outStream.ToArray()).Wait();
                outStream.Dispose();
            }
        }

        static async Task<string> rest(string filename)
        {
            var requestUri = "http://13.93.234.124:88/score";

            using (var client = new HttpClient())
            {
                string data = "\"{\\\"input\\\": \\\"https://yiri.blob.core.windows.net/soundid/" + filename + "\\\"}\"";
                Console.WriteLine(data);
                StringContent httpContent = new StringContent(data, Encoding.Default, "application/json");
                httpContent.Headers.ContentType = MediaTypeHeaderValue.Parse("application/json");
                HttpResponseMessage response = await client.PostAsync(requestUri, httpContent);
                string responseString = await response.Content.ReadAsStringAsync();
                return responseString;
            }
        }

        private async Task OnBinaryMessageReceived(WebSocketHandler handler, byte[] bytes, string conversationId, string watermark)
        {
            WaveFormat waveFormat = new WaveFormat(8000, 16, 2);
            MemoryStream uploadStream = new MemoryStream();
            using (WaveFileWriter writer = new WaveFileWriter(uploadStream, waveFormat))
            {
                writer.WriteData(bytes, 0, bytes.Length);
                CloudStorageAccount storageAccount = CloudStorageAccount.Parse("DefaultEndpointsProtocol=https;AccountName=yiri;AccountKey=s7jeE5QYui4tKdY6olEXMpAytRBTz35WLaMfZazZjONsLWirCy+JCcvwC7lQwcFmrPFuE16HHzdigws+cUPAHQ==;EndpointSuffix=core.windows.net");
                CloudBlobClient blobClient = storageAccount.CreateCloudBlobClient();
                CloudBlobContainer container = blobClient.GetContainerReference("soundid");
                string fileName = Guid.NewGuid().ToString() + ".wav";
                CloudBlockBlob blockBlob = container.GetBlockBlobReference(fileName);
                uploadStream.Position = 0;
                blockBlob.UploadFromStream(uploadStream, bytes.Length + 44);
                await handler.SendMessage(await rest(fileName));
            }
            return;
        }
    }
}
