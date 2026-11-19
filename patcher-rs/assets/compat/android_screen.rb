# Added by the MKXP-Z Patcher (Android APKs).
# On Android the game always fills the screen: requests from the game's
# scripts to leave fullscreen or to resize the window (e.g. Fullscreen++'s
# windowed mode from Game.ini, a simulated Alt+Enter) are ignored. Leaving
# fullscreen would bring back Android's status and navigation bars.

module MkxpzAndroidScreen
  def self.android?
    ENV['ANDROID_ROOT'] && ENV['ANDROID_DATA'] ? true : false
  end
end

if MkxpzAndroidScreen.android?
  class << Graphics
    alias_method :mkxpz_android_set_fullscreen, :fullscreen=
    def fullscreen=(_value)
      mkxpz_android_set_fullscreen(true) unless fullscreen
    end

    def resize_window(*_args) end
    def center(*_args) end
  end
end
