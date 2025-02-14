/** Copyright (c) 2021 Nikolai Wuttke
  *
  * Permission is hereby granted, free of charge, to any person obtaining a copy
  * of this software and associated documentation files (the "Software"), to deal
  * in the Software without restriction, including without limitation the rights
  * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  * copies of the Software, and to permit persons to whom the Software is
  * furnished to do so, subject to the following conditions:
  *
  * The above copyright notice and this permission notice shall be included in all
  * copies or substantial portions of the Software.
  *
  * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  * SOFTWARE.
  */

#include "view.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include <cxxopts.hpp>
#include <GLES2/gl2.h>
#include <SDL.h>

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <optional>


namespace
{

std::optional<cxxopts::ParseResult> parseArgs(int argc, char** argv)
{
  try
  {
    cxxopts::Options options(argv[0], "TvTextViewer - a full-screen text viewer");

    options
      .positional_help("[input file]")
      .show_positional_help()
      .add_options()
        ("input_file", "text file to view", cxxopts::value<std::string>())
        ("s,script_file", "script outpout to view", cxxopts::value<std::string>())
        ("m,message", "text to show instead of viewing a file", cxxopts::value<std::string>())
        ("f,font_size", "font size in pixels", cxxopts::value<int>())
        ("t,title", "window title (filename by default)", cxxopts::value<std::string>())
        ("y,yes_button", "shows a yes button with different exit code")
        ("e,error_display", "format as error, background will be red")
        ("w,wrap_lines", "wrap long lines of text. WARNING: could be slow for large files!")
        ("h,help", "show help")
      ;

    options.parse_positional({"input_file"});

    try
    {
      const auto result = options.parse(argc, argv);

      if (result.count("help"))
      {
        std::cout << options.help({""}) << '\n';
        std::exit(0);
      }

      if (!result.count("input_file") && !result.count("message") && !result.count("script_file"))
      {
        std::cerr << "Error: No input given\n\n";
        std::cerr << options.help({""}) << '\n';
        return {};
      }

      if (result.count("input_file") && result.count("message"))
      {
        std::cerr << "Error: Cannot use input_file and message at the same time\n\n";
        std::cerr << options.help({""}) << '\n';
        return {};
      }

      return result;
    }
    catch (const cxxopts::OptionParseException& e)
    {
      std::cerr << "Error: " << e.what() << "\n\n";
      std::cerr << options.help({""}) << '\n';
    }
  }
  catch (const cxxopts::OptionSpecException& e)
  {
    std::cerr << "Error defining options: " << e.what() << '\n';
  }

  return {};
}


std::string replaceEscapeSequences(const std::string& original)
{
  std::string result;
  result.reserve(original.size());

  for (auto iChar = original.begin(); iChar != original.end(); ++iChar)
  {
    if (*iChar == '\\' && std::next(iChar) != original.end())
    {
      switch (*std::next(iChar))
      {
        case 'f': result.push_back('\f'); ++iChar; break;
        case 'n': result.push_back('\n'); ++iChar; break;
        case 'r': result.push_back('\r'); ++iChar; break;
        case 't': result.push_back('\t'); ++iChar; break;
        case 'v': result.push_back('\v'); ++iChar; break;
        case '\\': result.push_back('\\'); ++iChar; break;

        default:
          result.push_back(*iChar);
          break;
      }
    }
    else
    {
      result.push_back(*iChar);
    }
  }

  return result;
}


std::string readInputOrScriptName(const cxxopts::ParseResult& args)
{
  if (args.count("input_file"))
  {
    const auto& inputFilename = args["input_file"].as<std::string>();
    std::ifstream file(inputFilename, std::ios::ate);
    if (!file.is_open())
    {
      return {};
    }

    const auto fileSize = file.tellg();
    file.seekg(0);

    std::string inputText;
    inputText.resize(fileSize);
    file.read(&inputText[0], fileSize);

    return inputText;
  }
  else if (args.count("script_file"))
  {
    return args["script_file"].as<std::string>();
  }
  else
  {
    return replaceEscapeSequences(args["message"].as<std::string>());
  }
}


std::string determineTitle(const cxxopts::ParseResult& args)
{
  if (args.count("title"))
  {
    return args["title"].as<std::string>();
  }
  else if (args.count("input_file"))
  {
    return args["input_file"].as<std::string>();
  }
  else if (args.count("error_display"))
  {
    return "Error!!";
  }
  else
  {
    return "Info";
  }
}


int run(SDL_Window* pWindow, const cxxopts::ParseResult& args)
{
  std::vector<SDL_GameController*> gameControllers;

  auto clearGameControllers = [&]()
  {
    for (const auto pController : gameControllers)
    {
      SDL_GameControllerClose(pController);
    }

    gameControllers.clear();
  };

  auto enumerateGameControllers = [&]()
  {
    clearGameControllers();

    for (std::uint8_t i = 0; i < SDL_NumJoysticks(); ++i) {
      if (SDL_IsGameController(i)) {
        gameControllers.push_back(SDL_GameControllerOpen(i));
      }
    }
  };


  auto view = View{
    determineTitle(args),
    readInputOrScriptName(args),
    args.count("yes_button") > 0,
    args.count("wrap_lines") > 0,
    args.count("script_file") > 0};

  const auto& io = ImGui::GetIO();

  std::optional<int> exitCode;
  while (!exitCode)
  {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (
        event.type == SDL_QUIT ||
        (event.type == SDL_CONTROLLERBUTTONDOWN &&
         (event.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)) ||
        (event.type == SDL_WINDOWEVENT &&
         event.window.event == SDL_WINDOWEVENT_CLOSE &&
         event.window.windowID == SDL_GetWindowID(pWindow))
      ) {
        return 0;
      }

      if (
        event.type == SDL_CONTROLLERDEVICEADDED ||
        event.type == SDL_CONTROLLERDEVICEREMOVED)
      {
        enumerateGameControllers();
      }
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame(pWindow, gameControllers);
    ImGui::NewFrame();

    // Draw the UI
    exitCode = view.draw(io.DisplaySize);

    // Rendering
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(pWindow);
  }

  return *exitCode;
}

}


int main(int argc, char** argv)
{
  const auto oArgs = parseArgs(argc, argv);
  if (!oArgs)
  {
    return -2;
  }

  const auto& args = *oArgs;


  if (const auto dbFilePath = SDL_getenv("SDL_GAMECONTROLLERCONFIG_FILE"))
  {
    if (SDL_GameControllerAddMappingsFromFile(dbFilePath) >= 0)
    {
      std::cout << "Game controller mappings loaded\n";
    }
    else
    {
      std::cerr
        << "Could not load controller mappings from file '"
        << dbFilePath << "': " << SDL_GetError() << '\n';
    }
  }

  // Setup SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
  {
    std::cerr << "Error: " << SDL_GetError() << '\n';
    return -1;
  }

  // Setup window and OpenGL
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_DisplayMode displayMode;
  SDL_GetDesktopDisplayMode(0, &displayMode);

  auto pWindow = SDL_CreateWindow(
    "Log Viewer",
    SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED,
    displayMode.w,
    displayMode.h,
    SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);

  auto pGlContext = SDL_GL_CreateContext(pWindow);
  SDL_GL_MakeCurrent(pWindow, pGlContext);
  SDL_GL_SetSwapInterval(1); // Enable vsync

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.Fonts->AddFontFromFileTTF("/storage/.config/retroarch/regular.ttf", 50.0f, NULL, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

  // Disable creation of imgui.ini
  io.IniFilename = nullptr;

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  if (args.count("error_display")) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(ImColor(94, 11, 22, 255))); // Set window background to red
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(ImColor(94, 11, 22, 255)));
  }

  if (args.count("font_size"))
  {
    ImFontConfig config;
    config.SizePixels = args["font_size"].as<int>();
    ImGui::GetIO().Fonts->AddFontDefault(&config);
    //io.Fonts->AddFontFromFileTTF("/storage/.config/retroarch/regular.ttf", 13.0f, NULL, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
  }

  // Setup Platform/Renderer bindings
  ImGui_ImplSDL2_InitForOpenGL(pWindow, pGlContext);
  ImGui_ImplOpenGL3_Init(nullptr);

  // Main loop
  const auto exitCode = run(pWindow, args);

  // Cleanup
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(pGlContext);
  SDL_DestroyWindow(pWindow);
  SDL_Quit();

  return exitCode;
}
