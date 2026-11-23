# MKXP-Z for the 3DS: the Ruby side of the RGSS classes, and the start of
# the game (its scripts, like RPG Maker's player runs them).
# $MKXPZ_RGSS (1 XP, 2 VX, 3 VX Ace) and $MKXPZ_SCRIPTS are set by main.c.

class RGSSError < StandardError; end
class RGSSReset < Exception; end

class Font
  class << self
    attr_accessor :default_name, :default_size, :default_bold, :default_italic,
                  :default_outline, :default_shadow, :default_color, :default_out_color
  end
  attr_accessor :name, :size, :bold, :italic, :outline, :shadow, :color, :out_color

  def initialize(name = Font.default_name, size = Font.default_size)
    @name = name.is_a?(String) ? name.dup : name
    @size = size
    @bold = Font.default_bold
    @italic = Font.default_italic
    @outline = Font.default_outline
    @shadow = Font.default_shadow
    @color = Font.default_color.dup
    @out_color = Font.default_out_color.dup
  end

  def initialize_copy(other)
    super
    @color = other.color.dup
    @out_color = other.out_color.dup
  end

  def color=(c)
    @color = c.dup
  end

  def out_color=(c)
    @out_color = c.dup
  end
end

case $MKXPZ_RGSS
when 3
  Font.default_name = ['VL Gothic']
  Font.default_size = 24
  Font.default_outline = true
  Font.default_shadow = false
when 2
  Font.default_name = ['UmePlus Gothic', 'MS PGothic', 'VL PGothic', 'VL Gothic']
  Font.default_size = 20
  Font.default_outline = false
  Font.default_shadow = true
else
  Font.default_name = ['MS PGothic', 'VL PGothic', 'VL Gothic']
  Font.default_size = 22
  Font.default_outline = false
  Font.default_shadow = false
end
Font.default_bold = false
Font.default_italic = false
Font.default_color = Color.new(255, 255, 255, 255)
Font.default_out_color = Color.new(0, 0, 0, 128)

# Windows functions: none on the 3DS; calls do nothing, except Windows'
# MCI for videos (mciSendString: "open <file> ... alias X", "play X",
# "status X mode", "close X"; Pocket Mirror's video script): played with
# Graphics.play_movie, which returns when the video ends (or B skips it)
class Win32API
  def initialize(dll = nil, func = nil, *)
    @func = func.to_s
  end

  def call(*args)
    return 0 unless @func =~ /\AmciSendString/i
    Win32API.mkxpz_mci(args[0].to_s, args[1])
  end
  alias Call call

  def self.mkxpz_mci(cmd, buffer)
    @mci ||= {}
    case cmd
    when /\Aopen\s+"?(.+?)"?(?:\s+type\s+\S+)?\s+alias\s+(\S+)/i
      @mci[$2.downcase] = $1
    when /\Aplay\s+(\S+)/i
      path = @mci[$1.downcase]
      Graphics.play_movie(path) if path
    when /\Astatus\s+\S+\s+mode/i
      buffer.replace('stopped') if buffer.is_a?(String)
    when /\Aclose\s+(\S+)/i
      @mci.delete($1.downcase)
    end
    0
  end
end

# Screenshots in save files ("快速存储Bitmap的Marshal" by 柳之一, e.g. in
# Changed): the script's Bitmap#_dump / Bitmap._load copy the original
# engine's pixel memory with RtlMoveMemory, which doesn't exist here (the
# screenshot came out empty). Replaced, when a script defines them with its
# RtlMoveMemory constants, by versions with the real pixels in the original
# format (BGRA, bottom row first, zlib): saves stay compatible with the PC
# (Game.exe and the patcher's mkxp-z: its compat/bitmap_marshal.rb).
module MkxpzBitmapMarshal
  def self.dump(bmp)
    [bmp.width, bmp.height, Zlib::Deflate.deflate(bmp.mkxpz_bgra_bottom_up)].pack('LLa*')
  end

  def self.load(klass, str)
    w, h, zdata = str.unpack('LLa*')
    b = klass.new([w, 1].max, [h, 1].max)
    begin
      b.mkxpz_load_bgra_bottom_up(Zlib::Inflate.inflate(zdata))
    rescue StandardError => e
      mkxpz_log("save file image unreadable: #{e.message}")
    end
    b
  end

  def self.memory_script?
    Bitmap.constants.any? { |c| c.to_s =~ /RtlMoveMemory/i }
  rescue StandardError
    false
  end

  module Impl
    def _dump(limit)
      MkxpzBitmapMarshal.dump(self)
    end
  end

  module ClassImpl
    def _load(str)
      MkxpzBitmapMarshal.load(self, str)
    end
  end

  def self.install(target, name, impl)
    return if @installing || !memory_script?
    @installing = true
    begin
      target.send(:define_method, name, impl.instance_method(name))
      mkxpz_log("screenshots in saves: Bitmap.#{name} with real pixels")
    ensure
      @installing = false
    end
  end
end

class Bitmap
  def self.method_added(name)
    super
    MkxpzBitmapMarshal.install(self, :_dump, MkxpzBitmapMarshal::Impl) if name == :_dump && self == Bitmap
  end

  def self.singleton_method_added(name)
    super
    MkxpzBitmapMarshal.install(singleton_class, :_load, MkxpzBitmapMarshal::ClassImpl) if name == :_load && self == Bitmap
  end
end

# XP: not drawn yet; maps are empty (VX / VX Ace: tilemap.c)
unless defined?(Tilemap)
class Tilemap
  attr_accessor :bitmaps, :map_data, :flash_data, :flags, :viewport, :visible, :ox, :oy,
                :tileset, :autotiles, :priorities
  def initialize(viewport = nil)
    @viewport = viewport
    @bitmaps = []
    @autotiles = []
    @visible = true
    @ox = @oy = 0
    @disposed = false
  end
  def dispose; @disposed = true; end
  def disposed?; @disposed; end
  def update; end
end
end

module Kernel
  def load_data(filename)
    Graphics.mkxpz_loading
    File.open(filename, 'rb') { |f| Marshal.load(f) }
  ensure
    Graphics.mkxpz_loading
  end

  def save_data(obj, filename)
    File.open(filename, 'wb') { |f| Marshal.dump(obj, f) }
  end

  def msgbox(*args)
    $stdout.puts(args.map(&:to_s).join)
    mkxpz_log("message box: #{args.map(&:to_s).join}") if respond_to?(:mkxpz_log, true)
    nil
  end

  def msgbox_p(*args)
    $stdout.puts(args.map(&:inspect).join("\n"))
    nil
  end

  def rgss_main
    begin
      yield
    rescue RGSSReset
      mkxpz_reset_cleanup
      retry
    end
  end

  # as mkxp-z does on a reset: what's on the screen goes, the sound stops
  def mkxpz_reset_cleanup
    mkxpz_dispose_all
    Graphics.mkxpz_reset
    Audio.bgm_stop; Audio.bgs_stop; Audio.me_stop; Audio.se_stop
    GC.start
  end

  # the scripts; a reset outside rgss_main (XP and VX games have none: F12
  # ran all their scripts again) runs them again, from inside the rescue
  # like RGSS1/2 did: $! and $@ are set then, which scripts check so they
  # don't alias their methods twice ("alias ... unless $@")
  def mkxpz_run_all
    mkxpz_run_scripts
  rescue RGSSReset
    mkxpz_log("reset: running the scripts again")
    mkxpz_reset_cleanup
    mkxpz_run_all
  end

  def rgss_stop
    loop { Graphics.update }
  end
end
# --- what RGSS1-3 had from Ruby 1.8 / 1.9 (as MKXP-Z's ruby_classic_wrap.rb)
class Hash
  alias_method :index, :key unless method_defined?(:index)
end
class Object
  TRUE = true unless const_defined?(:TRUE)
  FALSE = false unless const_defined?(:FALSE)
  NIL = nil unless const_defined?(:NIL)
  alias_method :id, :object_id unless method_defined?(:id)
  alias_method :type, :class unless method_defined?(:type)
end
class NilClass
  def id; 4; end
end
class TrueClass
  def id; 2; end
end
# Ruby 1.8: string[i] = 65 set a character code (Heartache 101's input
# script); later Ruby raises a TypeError for it
class String
  alias_method :mkxpz_classic_aset, :[]=
  def []=(*args)
    args[-1] = (args[-1] & 255).chr if args.size >= 2 && args[-1].is_a?(Integer)
    mkxpz_classic_aset(*args)
  end
end

# Windows' text mode: reading a file opened with "r" (not "rb") turned
# CRLF line ends into "\n" (games compare lines with "\n", Heartache 101's
# profile files have CRLF); binary reads stay as they are
class File
  class << self
    def mkxpz_text_read?(mode, opts)
      mode = opts[:mode] if opts.key?(:mode)
      mode.nil? || (mode.is_a?(String) && mode.start_with?('r') && !mode.include?('b') && !mode.include?('+') &&
                    !mode.include?(':'))
    end
    # (Ruby data, e.g. save files, which VX reads with "r": never; it
    # starts with 04 08, and CRLF conversion broke it: no saves seen)
    def mkxpz_marshal_file?(name)
      name.is_a?(String) && (File.binread(name, 2) == "\x04\x08".b rescue false)
    end
    alias_method :mkxpz_text_open, :open
    def open(name, mode = nil, *rest, **opts, &blk)
      if mkxpz_text_read?(mode, opts) && !opts.key?(:universal_newline) && !opts.key?(:binmode) && rest.empty? &&
         !mkxpz_marshal_file?(name)
        opts = opts.merge(universal_newline: true)
      end
      mode ? mkxpz_text_open(name, mode, *rest, **opts, &blk) : mkxpz_text_open(name, **opts, &blk)
    end
  end
end
class IO
  class << self
    %i[read readlines foreach].each do |m|
      alias_method :"mkxpz_text_#{m}", m
      define_method(m) do |name, *args, **opts, &blk|
        # (File.read etc. without a mode: text, as on Windows)
        if self <= File && !opts.key?(:mode) && !opts.key?(:binmode) && !opts.key?(:universal_newline) &&
           name.is_a?(String) && !File.mkxpz_marshal_file?(name)
          opts = opts.merge(universal_newline: true)
        end
        send(:"mkxpz_text_#{m}", name, *args, **opts, &blk)
      end
    end
  end
end

# --- MKXP-Z's own additions (System, Graphics.delta, Input.release?...),
# for games written for MKXP-Z (Pokemon Essentials v21+): each only where
# the game doesn't define it itself. On the 3DS: no keyboard, mouse or
# window, so those answer "nothing".
module System
  MKXPZ_START = Process.clock_gettime(Process::CLOCK_MONOTONIC) rescue Time.now.to_f
  class << self
    def uptime
      (Process.clock_gettime(Process::CLOCK_MONOTONIC) rescue Time.now.to_f) - MKXPZ_START
    end
    def platform; 'Nintendo 3DS'; end
    def is_windows?; false; end
    def is_mac?; false; end
    def is_linux?; false; end
    def is_really_windows?; false; end
    def is_really_mac?; false; end
    def is_really_linux?; false; end
    def is_rosetta?; false; end
    def is_wine?; false; end
    def user_language; 'en_US'; end
    def user_name; 'Player'; end
    def data_directory; './'; end
    def game_title
      @mkxpz_title ||= (File.read('Game.ini')[/^\s*Title\s*=\s*(.*?)\s*$/i, 1] rescue nil).to_s
    end
    def mount(*) nil; end
    def unmount(*) nil; end
    def reload_cache(*) nil; end
    def set_window_title(*) nil; end
    def show_settings(*) nil; end
    def desensitize(name); name; end
    def nproc; 2; end
    def memory; 64; end
    def launch(*) nil; end
    def default_font_family; Font.default_name; end
    def power_state; { discharging: true, seconds: nil, percent: nil }; end
    def puts(*a) mkxpz_log(a.join("\n")); end
    def raw_key_states; ''; end
  end
  CONFIG = {} unless const_defined?(:CONFIG)
  VERSION = '2.4.2' unless const_defined?(:VERSION)
end

module Graphics
  class << self
    def mkxpz_compat_def(name, &blk)
      define_method(name, &blk) unless method_defined?(name)
    end
  end
  # seconds since the last Graphics.update (MKXP-Z), set by the update below
  @mkxpz_delta = 1.0 / 60
  class << self
    alias_method :mkxpz_compat_update, :update
    def update
      now = System.uptime
      @mkxpz_delta = @mkxpz_last ? now - @mkxpz_last : 1.0 / 60
      @mkxpz_last = now
      Input.mkxpz_compat_track if Input.respond_to?(:mkxpz_compat_track)
      mkxpz_compat_update
    end
    def delta; @mkxpz_delta; end
    def fullscreen; false; end
    def fullscreen=(v); v; end
    def center; nil; end
    def scale; 1.0; end
    def scale=(v); v; end
    def frame_time; @mkxpz_delta; end
    def average_frame_rate; frame_rate; end
    def display_width; 400; end
    def display_height; 240; end
    def show_cursor; false; end
    def show_cursor=(v); v; end
    def fixed_aspect_ratio; true; end
    def fixed_aspect_ratio=(v); v; end
    def smooth_scaling; 1; end
    def smooth_scaling=(v); v; end
    def integer_scaling; false; end
    def integer_scaling=(v); v; end
    def screenshot(*) false; end
    def vsync; true; end
    def vsync=(v); v; end
  end
end

module Input
  class << self
    # how long each button has been held (frames) and what was held in the
    # last frame: from the first use of release? / time? / count on
    def mkxpz_compat_track
      return unless @mkxpz_track
      @mkxpz_prev = @mkxpz_now || {}
      @mkxpz_now = {}
      @mkxpz_frames ||= Hash.new(0)
      @mkxpz_track.each_key do |k|
        held = press?(k)
        @mkxpz_now[k] = held
        @mkxpz_frames[k] = held ? @mkxpz_frames[k] + 1 : 0
      end
    end
    def mkxpz_watch(k)
      (@mkxpz_track ||= {})[k] = true
    end
    def release?(k)
      mkxpz_watch(k)
      (@mkxpz_prev || {})[k] && !press?(k) ? true : false
    end
    def count(k)
      mkxpz_watch(k)
      (@mkxpz_frames || {})[k].to_i
    end
    def time?(k)
      count(k) / 60.0
    end
    def pressex?(*) false; end
    def triggerex?(*) false; end
    def repeatex?(*) false; end
    def releaseex?(*) false; end
    def repeatcount(k); count(k); end
    def timeex?(*) 0.0; end
    def text_input; false; end
    def text_input=(v); v; end
    def gets; ''; end
    def clipboard; ''; end
    def clipboard=(v); v; end
    def scroll_v; 0; end
    def mouse_x; 0; end
    def mouse_y; 0; end
    def mouse_in_window; false; end
    def key_name(k); k.to_s; end
    def axis_triggerex?(*) false; end
    def joystick; nil; end
    def rumble(*) nil; end
  end
end

class Bitmap
  def self.max_size; 1024; end unless respond_to?(:max_size)
end

# VX Ace's RGSS3 (and MKXP-Z) have it; games and the widescreen script
# check it
RGSS_VERSION = '3.0.1' if $MKXPZ_RGSS == 3 && !defined?(RGSS_VERSION)

# XP / VX print with message boxes too (RGSS1's print is one as well:
# Middens shows "Unable to find file" with it)
alias print msgbox

$stdout.sync = true

# >>> mkxpz-fast-helpers (tools/fast-checksums.rb uses these too)
# a method's text, without comments and indentation
def mkxpz_norm(text)
  text.lines.map { |l| l.sub(/(\A|\s)#.*/, '').strip }.reject(&:empty?).join("\n")
end

# the text of the method defined at line of a script section: up to the
# first "end" with the indentation of its "def"
def mkxpz_method_text(script, line)
  l = script.lines
  d = l[line - 1] or return nil
  ind = d[/\A */]
  j = (line...l.size).find { |k| l[k] =~ /\A#{ind}end\b/ } or return nil
  l[(line - 1)..j].join
end
# <<< mkxpz-fast-helpers

# Faster standard VX Ace scripts: Sprite_Character#update runs in C
# (scene.c), but only while every method it stands for is the standard one.
# Checksums of the standard methods' text (ports/3ds/tools/fast-checksums.rb);
# :attr an attr_reader/accessor, :c the engine's own.
MKXPZ_STOCK = {
  'Sprite_Character#update' => 2558777199,
  'Sprite_Character#update_bitmap' => 4103765253,
  'Sprite_Character#graphic_changed?' => 3797258696,
  'Sprite_Character#update_src_rect' => 1942911822,
  'Sprite_Character#update_position' => 1236191671,
  'Sprite_Character#update_other' => 1919106263,
  'Sprite_Character#update_balloon' => 3185305667,
  'Sprite_Character#setup_new_effect' => 3675786357,
  'Sprite_Base#update' => 1901904939,
  'Sprite_Base#animation?' => 4013398038,
  'Game_CharacterBase#screen_x' => 1265888610,
  'Game_CharacterBase#screen_y' => 414283810,
  'Game_CharacterBase#screen_z' => 16537052,
  'Game_CharacterBase#shift_y' => 3177371100,
  'Game_CharacterBase#jump_height' => 1514128538,
  'Game_CharacterBase#object_character?' => 2074604390,
  'Game_CharacterBase#update' => 3737941549,
  'Game_CharacterBase#update_stop' => 3926629533,
  'Game_CharacterBase#update_animation' => 2588247382,
  'Game_CharacterBase#update_anime_count' => 2986621416,
  'Game_CharacterBase#moving?' => 3458338668,
  'Game_CharacterBase#jumping?' => 4224591806,
  'Game_CharacterBase#real_move_speed' => 680567531,
  'Game_CharacterBase#dash?' => 1832952089,
  'Game_Map#adjust_x' => 1413106839,
  'Game_Map#adjust_y' => 3928380013,
  'Game_Map#loop_horizontal?' => 812218791,
  'Game_Map#loop_vertical?' => 975953756,
  'Game_Map#screen_tile_x' => 1211119882,
  'Game_Map#screen_tile_y' => 3058309681,
  'Game_Map#width' => 4255624125,
  'Game_Map#height' => 4262117737,
  'Game_Event#refresh' => 2670169069,
  'Game_Event#find_proper_page' => 1545490489,
  'Game_Event#conditions_met?' => 3819658317,
  'Game_Event#update' => 2489914872,
  'Game_Event#update_stop' => 3832805855,
  'Game_Event#update_self_movement' => 2002458890,
  'Game_Event#near_the_screen?' => 2803408571,
  'Game_Event#stop_count_threshold' => 1758990067,
  'Game_Event#check_event_trigger_auto' => 1741691701,
  'Game_Character#update_stop' => 634477818,
  'Game_Switches#[]' => 1897994590,
  'Game_Variables#[]' => 3739613371,
  'Game_SelfSwitches#[]' => 3480227492,
  'Game_CharacterBase#tile_id' => :attr,
  'Game_CharacterBase#character_name' => :attr,
  'Game_CharacterBase#character_index' => :attr,
  'Game_CharacterBase#pattern' => :attr,
  'Game_CharacterBase#direction' => :attr,
  'Game_CharacterBase#opacity' => :attr,
  'Game_CharacterBase#blend_type' => :attr,
  'Game_CharacterBase#bush_depth' => :attr,
  'Game_CharacterBase#transparent' => :attr,
  'Game_CharacterBase#animation_id' => :attr,
  'Game_CharacterBase#balloon_id' => :attr,
  'Sprite#update' => :c,
  'Sprite#x=' => :c,
  'Sprite#y=' => :c,
  'Sprite#z=' => :c,
  'Sprite#opacity=' => :c,
  'Sprite#blend_type=' => :c,
  'Sprite#bush_depth=' => :c,
  'Sprite#visible=' => :c,
  'Sprite#src_rect' => :c,
}
MKXPZ_FAST_NEEDS = {
  # (not the methods the engine replaces, refresh and update: those are
  # checked once, before they're replaced; after that they're the engine's)
  event: %w[find_proper_page conditions_met?],
  event_update: %w[update_stop update_self_movement near_the_screen? stop_count_threshold
                   check_event_trigger_auto update_animation update_anime_count moving? jumping? real_move_speed dash?],
  switches: %w[[]],
  variables: %w[[]],
  self_switches: %w[[]],
  sprite: %w[update_bitmap graphic_changed? update_src_rect update_position update_other update_balloon
             setup_new_effect animation? x= y= z= opacity= blend_type= bush_depth= visible= src_rect],
  character: %w[screen_x screen_y screen_z shift_y jump_height object_character? tile_id character_name
                character_index pattern direction opacity blend_type bush_depth transparent animation_id balloon_id],
  map: %w[adjust_x adjust_y loop_horizontal? loop_vertical? screen_tile_x screen_tile_y width height],
}

def mkxpz_stock?(klass, name)
  m = klass.instance_method(name)
  exp = mkxpz_stock_table["#{m.owner}##{m.original_name}"] or return false
  file, line = m.source_location
  return file.nil? if exp == :c
  return file == 'module_rpg.rb' && RubyVM::InstructionSequence.of(m).nil? if exp == :rpg
  script = mkxpz_script_text(file) or return false
  return RubyVM::InstructionSequence.of(m).nil? && script.lines[line - 1].to_s =~ /\A\s*attr_(reader|accessor)\b/ if exp == :attr
  text = mkxpz_method_text(script, line) or return false
  Zlib.crc32(mkxpz_norm(text)) == exp
rescue NameError
  false
end
# the same for RPG Maker VX (RGSS2): Sprite_Character#update (scene.c
# fast_update_vx); checksums from VX's standard scripts
MKXPZ_STOCK_VX = {
  'Sprite_Character#update' => 1579953485,
  'Sprite_Character#update_bitmap' => 3188977104,
  'Sprite_Character#update_src_rect' => 1942911822,
  'Sprite_Character#update_balloon' => 3727418189,
  'Sprite_Base#update' => 208928782,
  'Game_Character#screen_x' => 4149964556,
  'Game_Character#screen_y' => 2379379738,
  'Game_Character#screen_z' => 283499415,
  'Game_Character#object?' => 1400731312,
  'Game_Map#adjust_x' => 2112496703,
  'Game_Map#adjust_y' => 269686746,
  'Game_Map#loop_horizontal?' => 3787205086,
  'Game_Map#loop_vertical?' => 729937964,
  'Game_Character#tile_id' => :attr,
  'Game_Character#character_name' => :attr,
  'Game_Character#character_index' => :attr,
  'Game_Character#pattern' => :attr,
  'Game_Character#direction' => :attr,
  'Game_Character#opacity' => :attr,
  'Game_Character#blend_type' => :attr,
  'Game_Character#bush_depth' => :attr,
  'Game_Character#transparent' => :attr,
  'Game_Character#animation_id' => :attr,
  'Game_Character#balloon_id' => :attr,
  'Game_Event#update' => 1817010945,
  'Game_Event#check_event_trigger_auto' => 1741691701,
  'Game_Character#update' => 4099721432,
  'Game_Character#update_stop' => 2946147759,
  'Game_Character#update_self_movement' => 792687214,
  'Game_Character#update_animation' => 1040953005,
  'Game_Character#jumping?' => 1865662889,
  'Game_Character#moving?' => 3734878286,
  'Game_Character#dash?' => 1832952089,
  'Game_Event#on_screen' => 1066362925, # ("Anti Lag VX" by Anaryu)
  'Game_Map#update_events' => 2215309251,
  'Game_Interpreter#setup_starting_event' => 796049156,
  'Game_Map#events' => :attr,
  'Game_Map#need_refresh' => :attr,
  'Game_Temp#common_event_id' => :attr,
  'Game_Event#starting' => :attr,
  'Game_Event#update?' => 4171932450, # ("Anti Lag VX")
  'Game_Character#on_old=' => :attr,
  'Game_Character#on_new' => :attr,
  'Sprite_Character#character' => :attr,
  'Game_Map#display_x' => :attr,
  'Game_Map#display_y' => :attr,
  'RPG::Map#scroll_type' => :rpg,
  'RPG::Map#width' => :rpg,
  'RPG::Map#height' => :rpg,
  'Sprite#update' => :c,
  'Sprite#x=' => :c,
  'Sprite#y=' => :c,
  'Sprite#z=' => :c,
  'Sprite#opacity=' => :c,
  'Sprite#blend_type=' => :c,
  'Sprite#bush_depth=' => :c,
  'Sprite#visible=' => :c,
  'Sprite#src_rect' => :c,
}
MKXPZ_FAST_NEEDS_VX = {
  sprite: %w[update_bitmap update_src_rect update_balloon x= y= z= opacity= blend_type= bush_depth= visible= src_rect],
  character: %w[screen_x screen_y screen_z object? tile_id character_name character_index pattern direction opacity
                blend_type bush_depth transparent animation_id balloon_id],
  map: %w[adjust_x adjust_y loop_horizontal? loop_vertical?],
  # (Game_Character#update itself: checked once at setup, Game_Event's own
  # update may be a script's wrapper around the standard one)
  event_update: %w[check_event_trigger_auto update_stop update_self_movement update_animation jumping? moving? dash?],
}

def mkxpz_stock_table
  $MKXPZ_RGSS == 2 ? MKXPZ_STOCK_VX : MKXPZ_STOCK
end

# a script's text by the file name Ruby reports: a section of
# Scripts.rvdata(2), or a file (games that load their scripts from files)
def mkxpz_script_text(file)
  return nil unless file
  return $mkxpz_sections[file] if $mkxpz_sections.key?(file)
  ($mkxpz_files ||= {})[file] ||= (File.exist?(file) ? File.read(file).force_encoding(Encoding::UTF_8) : false)
  $mkxpz_files[file] || nil
end

# (asked by the engine for each class it meets)
def mkxpz_fast_ok?(klass, kind)
  return false if kind == :sprite && !(klass <= Sprite_Character)
  needs = $MKXPZ_RGSS == 2 ? MKXPZ_FAST_NEEDS_VX : MKXPZ_FAST_NEEDS
  needs[kind].all? { |n| mkxpz_stock?(klass, n) }
rescue StandardError
  false
end

def mkxpz_fast_setup
  return mkxpz_fast_setup_vx if $MKXPZ_RGSS == 2
  return if $MKXPZ_RGSS != 3 || MKXPZ_STOCK.empty?
  return unless defined?(Sprite_Character) && defined?(Sprite_Base) && defined?(Game_Event) && defined?(Game_Map)
  return unless Sprite_Character.superclass == Sprite_Base && Sprite_Base.superclass == Sprite
  a = Sprite_Character.ancestors
  return unless a[1] == Sprite_Base && a[2] == Sprite
  return unless mkxpz_stock?(Sprite_Character, :update) && mkxpz_stock?(Sprite_Base, :update) && mkxpz_stock?(Sprite, :update)
  return unless mkxpz_fast_ok?(Sprite_Character, :sprite) && mkxpz_fast_ok?(Game_Event, :character) &&
                mkxpz_fast_ok?(Game_Map, :map)
  Sprite_Character.send(:alias_method, :mkxpz_slow_update, :update)
  mkxpz_fast_install(Sprite_Character)
  mkxpz_log('character sprites: native')
rescue StandardError => e
  mkxpz_log("character sprites: Ruby (#{e.class})")
end

def mkxpz_fast_setup_vx
  return unless defined?(Sprite_Character) && defined?(Sprite_Base) && defined?(Game_Event) && defined?(Game_Map)
  a = Sprite_Character.ancestors
  return unless a[1] == Sprite_Base && a[2] == Sprite
  return unless mkxpz_stock?(Sprite_Character, :update) && mkxpz_stock?(Sprite_Base, :update) && mkxpz_stock?(Sprite, :update)
  return mkxpz_log('character sprites: Ruby (not the standard VX methods)') unless
    mkxpz_fast_ok?(Sprite_Character, :sprite) && mkxpz_fast_ok?(Game_Event, :character) && mkxpz_fast_ok?(Game_Map, :map)
  Sprite_Character.send(:alias_method, :mkxpz_slow_update, :update)
  mkxpz_fast_install_vx(Sprite_Character)
  mkxpz_log('character sprites: native (VX)')
rescue StandardError => e
  mkxpz_log("character sprites: Ruby (#{e.class}: #{e.message})")
ensure
  mkxpz_fast_setup_antilag
  mkxpz_fast_setup_event_update_vx
  mkxpz_fast_setup_antilag_loops
  mkxpz_fast_setup_vx_map_loops
end

# VX's Game_Map#update_events and Game_Interpreter#setup_starting_event in
# C (events.c): every event's update called directly when it's one of the
# engine's, the events' @starting read in C
def mkxpz_fast_setup_vx_map_loops
  return unless defined?(Game_Map) && defined?(Game_Interpreter) && defined?(Game_Temp) && defined?(Game_Event)
  kind = 0
  if mkxpz_native?(Game_Event, :update, Game_Event)
    kind = $mkxpz_event_update_native || 0
  end
  um = mkxpz_stock?(Game_Map, :update_events)
  ss = mkxpz_stock?(Game_Interpreter, :setup_starting_event) && mkxpz_stock?(Game_Map, :events) &&
       mkxpz_stock?(Game_Map, :need_refresh) && mkxpz_stock?(Game_Temp, :common_event_id) &&
       mkxpz_stock?(Game_Event, :starting)
  return unless um || ss
  Game_Map.send(:alias_method, :mkxpz_slow_update_events, :update_events) if um
  if ss
    Game_Interpreter.class_eval do
      alias_method :mkxpz_slow_setup_starting_event, :setup_starting_event
      def mkxpz_start_event(event)
        event.clear_starting
        setup(event.list, event.id)
      end
      # the end of the standard setup_starting_event: autorun common events
      def mkxpz_setup_starting_rest
        for event in $data_common_events.compact
          if event.trigger == 1 and $game_switches[event.switch_id] == true
            setup(event.list)
            return
          end
        end
      end
    end
  end
  temp = $game_temp ? $game_temp.class : Game_Temp
  mkxpz_fast_install_vx_map_loops(um ? Game_Map : nil, ss ? Game_Interpreter : nil, temp, Game_Event, kind)
  mkxpz_log("map loops: native#{' update_events' if um}#{' setup_starting_event' if ss} (event update kind #{kind})")
rescue StandardError => e
  mkxpz_log("map loops: Ruby (#{e.class}: #{e.message})")
end

# the checksum of a method's text (nil: no Ruby text to read)
def mkxpz_crc_of(klass, name)
  m = klass.instance_method(name)
  file, line = m.source_location
  script = mkxpz_script_text(file) or return nil
  text = mkxpz_method_text(script, line) or return nil
  Zlib.crc32(mkxpz_norm(text))
rescue NameError
  nil
end

# a method of the engine's (C, no source) defined right in owner
def mkxpz_native?(klass, name, owner)
  m = klass.instance_method(name)
  m.source_location.nil? && m.owner == owner
rescue NameError
  false
end

# (asked by events.c for each class its "Anti Lag VX" loops meet)
def mkxpz_antilag_ok?(klass, kind)
  case kind
  when :event
    klass <= Game_Event && mkxpz_native?(klass, :on_screen, Game_Event) &&
      mkxpz_native?(klass, :pre_antilag_update, Game_Event) && mkxpz_stock?(klass, :update?)
  when :attrs
    klass <= Game_Character && mkxpz_stock?(klass, :on_old=) && mkxpz_stock?(klass, :on_new)
  when :sprite
    klass <= Sprite_Character && mkxpz_native?(klass, :update, Sprite_Character) && mkxpz_stock?(klass, :character)
  else
    false
  end
rescue StandardError
  false
end

# "Anti Lag VX"'s Game_Event#update and Spriteset_Map#update_characters in C
# (events.c), with its own checksums (Game_Event#update in the table is
# VX's standard one, which the script keeps as pre_antilag_update)
def mkxpz_fast_setup_antilag_loops
  return unless defined?(Game_Event) && defined?(Spriteset_Map)
  ev = mkxpz_native?(Game_Event, :pre_antilag_update, Game_Event) &&
       mkxpz_native?(Game_Event, :on_screen, Game_Event) &&
       mkxpz_crc_of(Game_Event, :update) == 3260872479 && mkxpz_stock?(Game_Event, :update?)
  ss = Spriteset_Map.method_defined?(:update_characters) &&
       mkxpz_crc_of(Spriteset_Map, :update_characters) == 2370451155
  return unless ev || ss
  Game_Event.send(:alias_method, :mkxpz_slow_antilag_update, :update) if ev
  Spriteset_Map.send(:alias_method, :mkxpz_slow_update_characters, :update_characters) if ss
  mkxpz_fast_install_antilag_loops(ev ? Game_Event : nil, ss ? Spriteset_Map : nil)
  $mkxpz_event_update_native = 1 if ev
  mkxpz_log("anti lag: native#{' event update' if ev}#{' sprite loop' if ss}")
rescue StandardError => e
  mkxpz_log("anti lag loops: Ruby (#{e.class}: #{e.message})")
end

# Game_Event#update for standing events in C (scene.c fast_event_update_vx),
# under every name holding VX's standard Game_Event#update ("Anti Lag VX"
# keeps it as pre_antilag_update and calls it from its own update)
def mkxpz_fast_setup_event_update_vx
  return unless defined?(Game_Event) && Game_Event.superclass == Game_Character
  names = (Game_Event.instance_methods(false) + Game_Event.private_instance_methods(false)).select do |n|
    m = Game_Event.instance_method(n)
    # (not the engine's own copies: after a reset, mkxpz_slow_event_update
    # is still there and looks like the standard update; taking it made the
    # native update call itself until the stack ran out)
    !n.to_s.start_with?('mkxpz_') && m.original_name == :update && m.owner == Game_Event &&
      mkxpz_stock?(Game_Event, n)
  end
  if names.empty? && $mkxpz_event_update_names &&
     $mkxpz_event_update_names.all? { |n| Game_Event.method_defined?(n, false) && Game_Event.instance_method(n).source_location.nil? }
    # after a reset: the scripts kept their aliases ("unless $@"), and so
    # the native methods installed before
    return mkxpz_log("events: native update (VX: #{$mkxpz_event_update_names.join(', ')}, kept after the reset)")
  end
  return mkxpz_log('events: Ruby update (not the standard VX method)') if names.empty?
  return mkxpz_log('events: Ruby update (methods changed)') unless mkxpz_fast_ok?(Game_Event, :event_update) &&
                                                                     mkxpz_stock?(Game_Character, :update)
  Game_Event.class_eval do
    alias_method :mkxpz_slow_event_update, names.first
    # the end of the standard Game_Event#update (its event script)
    def mkxpz_update_interpreter
      @interpreter.setup(@list, @event.id) unless @interpreter.running?
      @interpreter.update
    end
  end
  names.each { |n| mkxpz_fast_install_event_update_vx(Game_Event, n.to_s) }
  $mkxpz_event_update_names = names
  $mkxpz_event_update_native = 2 if names.include?(:update)
  mkxpz_log("events: native update (VX: #{names.join(', ')})")
rescue StandardError => e
  mkxpz_log("events: Ruby update (#{e.class}: #{e.message})")
end

# "Anti Lag VX": Game_Event#on_screen in C (events.c)
def mkxpz_fast_setup_antilag
  return unless defined?(Game_Event) && Game_Event.method_defined?(:on_screen) && defined?(Game_Map)
  return unless mkxpz_stock?(Game_Event, :on_screen) && mkxpz_stock?(Game_Map, :display_x) &&
                mkxpz_stock?(Game_Map, :display_y)
  Game_Event.send(:alias_method, :mkxpz_slow_on_screen, :on_screen)
  mkxpz_fast_install_antilag(Game_Event, $game_map ? $game_map.class : Game_Map)
  mkxpz_log('anti lag: native on_screen')
rescue StandardError => e
  mkxpz_log("anti lag: Ruby (#{e.class}: #{e.message})")
end

# Game_Event#refresh in C (events.c), with find_proper_page and
# conditions_met?, while those and the readers they use are the standard
# ones (each class is asked again by the engine: mkxpz_fast_ok?)
def mkxpz_fast_setup_events
  return if $MKXPZ_RGSS != 3 || MKXPZ_STOCK.empty? || !defined?(RPG::Event::Page::Condition)
  return unless defined?(Game_Event) && defined?(Game_Switches) && defined?(Game_Variables) && defined?(Game_SelfSwitches)
  readers = {
    RPG::Event => %w[id pages],
    RPG::Event::Page => %w[condition],
    RPG::Event::Page::Condition => %w[switch1_valid switch1_id switch2_valid switch2_id variable_valid variable_id
                                      variable_value self_switch_valid self_switch_ch item_valid actor_valid],
  }
  return unless readers.all? { |k, names| names.all? { |n| MKXPZ_STOCK["#{k}##{n}"] = :rpg; mkxpz_stock?(k, n) } }
  return unless mkxpz_stock?(Game_Event, :refresh) && mkxpz_fast_ok?(Game_Event, :event)
  Game_Event.send(:alias_method, :mkxpz_slow_refresh, :refresh)
  mkxpz_fast_install_events(Game_Event)
  mkxpz_log('events: native refresh')
  mkxpz_fast_setup_event_update
rescue StandardError => e
  mkxpz_log("events: Ruby (#{e.class})")
end

# Game_Event#update in C (scene.c) for events standing still; the super
# chain (Game_Character, Game_CharacterBase) must be the standard one too
def mkxpz_fast_setup_event_update
  return unless Game_Event.ancestors[1] == Game_Character && Game_Character.ancestors[1] == Game_CharacterBase
  return unless mkxpz_stock?(Game_Character, :update_stop) && mkxpz_stock?(Game_CharacterBase, :update_stop) &&
                mkxpz_stock?(Game_CharacterBase, :update) && Game_Character.instance_method(:update).owner == Game_CharacterBase
  return unless mkxpz_stock?(Game_Event, :update) && mkxpz_fast_ok?(Game_Event, :event_update) &&
                mkxpz_fast_ok?(Game_Map, :map)
  Game_Event.class_eval do
    alias_method :mkxpz_slow_event_update, :update
    # the end of the standard Game_Event#update (its event script)
    def mkxpz_update_interpreter
      @interpreter.setup(@list, @event.id) unless @interpreter.running?
      @interpreter.update
    end
  end
  mkxpz_fast_install_event_update(Game_Event)
  mkxpz_log('events: native update')
rescue StandardError => e
  mkxpz_log("events: Ruby update (#{e.class})")
end

# Message boxes over the whole top screen (settings: "Text boxes", with the
# fit screen mode): the engine draws the picture Graphics.mkxpz_margin wider
# on each side than the game's screen, and the message box (only it and its
# helper windows) reaches into those sides. Laid out again when the setting
# changes, once the box is closed.
# the method is boot.rb's own already (preparations applied again after a
# reset that ran the scripts again, without the game redefining it): an
# alias of it would call itself forever
def mkxpz_ours?(klass, name)
  loc = klass.instance_method(name).source_location
  loc && loc[0] == 'boot.rb'
end

def mkxpz_wide_setup
  return unless defined?(Window_Message) && Window_Message.method_defined?(:update)
  return if mkxpz_ours?(Window_Message, :update)
  Window_Message.class_eval do
    alias_method :mkxpz_wide_update, :update
    def update
      mkxpz_wide_fit
      mkxpz_wide_update
    end

    def mkxpz_wide_fit
      m = Graphics.mkxpz_margin
      # (XP's windows have no openness: shown is visible)
      shown = respond_to?(:openness) ? openness > 0 : visible
      return if @mkxpz_m == m || (@mkxpz_m && shown) || disposed?
      old = @mkxpz_m || 0
      @mkxpz_m = m
      self.mkxpz_wide = true
      self.x = x + old - m
      self.width = width - 2 * old + 2 * m
      if respond_to?(:create_contents, true)
        create_contents
      else
        # XP: the contents made in initialize, 16 pixels inside each edge
        contents.dispose if contents && !contents.disposed?
        self.contents = Bitmap.new(width - 32, height - 32)
      end
      if @back_bitmap && @back_sprite && respond_to?(:create_back_bitmap, true)
        # VX Ace's dimmed background, as wide as the box
        @back_bitmap.dispose
        create_back_bitmap
        @back_sprite.bitmap = @back_bitmap
      end
      if @back_sprite
        @back_sprite.x = x
        @back_sprite.mkxpz_wide = true
      end
      @gold_window.x += m - old if @gold_window && @gold_window.x + @gold_window.width >= Graphics.width + old
      [@gold_window, @choice_window, @number_window, @item_window].each do |w|
        w.mkxpz_wide = true if w.respond_to?(:mkxpz_wide=)
      end
    rescue StandardError => e
      mkxpz_log("wide message box: #{e.class}: #{e.message}")
    end
  end
  # number input: placed at the wide box's left edge (VX), so it may use
  # the sides too (its first digits were cut off)
  if defined?(Window_NumberInput) && !mkxpz_ours?(Window_NumberInput, :initialize)
    Window_NumberInput.class_eval do
      alias_method :mkxpz_wide_ni_initialize, :initialize
      def initialize(*args)
        mkxpz_wide_ni_initialize(*args)
        self.mkxpz_wide = true if respond_to?(:mkxpz_wide=)
      end
    end
  end
  # VX Ace's choices: at the box's right end
  if defined?(Window_ChoiceList) && Window_ChoiceList.method_defined?(:update_placement) &&
     !mkxpz_ours?(Window_ChoiceList, :update_placement)
    Window_ChoiceList.class_eval do
      alias_method :mkxpz_wide_update_placement, :update_placement
      def update_placement
        mkxpz_wide_update_placement
        self.x += Graphics.mkxpz_margin if x + width >= Graphics.width
      end
    end
  end
rescue StandardError => e
  mkxpz_log("wide message box: #{e.class}")
end

# Save probe (a file "mkxpz-savelog" in the game folder): logs each part of
# the save data as it's converted (Marshal), each map event, and each block
# written to the SD card, so a save that never finishes shows where it
# stopped (every log line is written out at once).
class MkxpzLoggedFile
  def initialize(f) @f = f; @n = 0 end
  def binmode; @f.binmode; self end
  def write(*s)
    s.each do |x|
      @n += x.bytesize
      mkxpz_log("save: writing #{x.bytesize} bytes (#{@n} in all)")
      @f.write(x)
      mkxpz_log("save: written")
    end
    s.sum(&:bytesize)
  end
end

def mkxpz_save_probe(contents)
  free = Graphics.mkxpz_speed[/free [0-9\/]+ KB/]
  mkxpz_log("save probe: #{contents.size} parts, #{free}")
  contents.each do |k, v|
    mkxpz_log("save probe: #{k} (#{v.class})...")
    if k == :map
      v.instance_variables.each do |iv|
        x = v.instance_variable_get(iv)
        mkxpz_log("save probe:   map #{iv} (#{x.class})...")
        if iv == :@events && x.is_a?(Hash)
          x.each do |id, e|
            mkxpz_log("save probe:     event #{id}...")
            mkxpz_log("save probe:     event #{id}: #{Marshal.dump(e).bytesize} bytes")
          end
        else
          mkxpz_log("save probe:   map #{iv}: #{Marshal.dump(x).bytesize} bytes")
        end
      end
    end
    mkxpz_log("save probe: #{k}: #{Marshal.dump(v).bytesize} bytes")
  end
  mkxpz_log("save probe: all parts converted")
end

def mkxpz_save_probe_setup
  return unless File.exist?('mkxpz-savelog') && defined?(DataManager) &&
                DataManager.respond_to?(:save_game_without_rescue)
  class << DataManager
    alias_method :mkxpz_probe_save, :save_game_without_rescue
    def save_game_without_rescue(index)
      mkxpz_log("save: slot #{index + 1}")
      mkxpz_save_probe(make_save_contents)
      File.open(make_filename(index), 'wb') do |file|
        f = MkxpzLoggedFile.new(file)
        $game_system.on_before_save
        Marshal.dump(make_save_header, f)
        Marshal.dump(make_save_contents, f)
        @last_savefile_index = index
      end
      mkxpz_log("save: done")
      true
    end
  end
  mkxpz_log('save probe on (mkxpz-savelog)')
rescue StandardError => e
  mkxpz_log("save probe: #{e.class}")
end

def mkxpz_run_scripts
  # no Ruby warnings, as in RPG Maker's player (after a reset that runs the
  # scripts again, "already initialized constant" for each of them)
  $VERBOSE = nil
  $RGSS_SCRIPTS = load_data($MKXPZ_SCRIPTS)
  mkxpz_widescreen_setup
  $RGSS_SCRIPTS.each_with_index do |s, i|
    s[3] = Zlib::Inflate.inflate(s[2]).force_encoding(Encoding::UTF_8)
  end
  test = File.exist?('mkxpz-test.rb')
  # section name (as Ruby reports it) -> text; nil for names used twice
  $mkxpz_sections = {}
  $RGSS_SCRIPTS.each_with_index do |s, i|
    name = s[1].to_s.force_encoding(Encoding::UTF_8)
    file = name.empty? ? "Section#{i}" : name
    $mkxpz_sections[file] = $mkxpz_sections.key?(file) ? nil : s[3]
  end
  fast = true
  # no "Main" section (the scripts come from files through a loader, e.g.
  # Changed): the preparations at the first Graphics.update / freeze /
  # transition instead (graphics.c), when the loader has defined everything
  unless $RGSS_SCRIPTS.any? { |s| s[1].to_s.force_encoding(Encoding::UTF_8).strip == 'Main' }
    $mkxpz_late_test = test
    Graphics.mkxpz_setup_later
    fast = test = false
  end
  $RGSS_SCRIPTS.each_with_index do |s, i|
    name = s[1].to_s.force_encoding(Encoding::UTF_8)
    if fast && name.strip == 'Main'
      mkxpz_prepare
      fast = false
    end
    # for testing: mkxpz-test.rb in the game folder, before Main (which doesn't return)
    if test && name.strip == 'Main'
      eval(File.read('mkxpz-test.rb'), TOPLEVEL_BINDING, 'mkxpz-test.rb')
      test = false
    end
    file = name.empty? ? "Section#{i}" : name
    # (the start's loading bar: 40 % .. 100 % over the sections)
    Graphics.mkxpz_startup(0.4 + 0.6 * i / $RGSS_SCRIPTS.size) if i % 8 == 0 && Graphics.respond_to?(:mkxpz_startup)
    begin
      eval(s[3], TOPLEVEL_BINDING, file)
    rescue SyntaxError => e
      # (the section itself doesn't compile: nothing of it ran yet)
      raise unless e.message.start_with?("#{file}:")
      tries = (tries || 0) + 1
      fixed = mkxpz_ruby18_fix(s[3], e.message, file)
      raise if fixed == s[3] || tries > 5
      mkxpz_log("#{file}: Ruby 1.8 syntax rewritten")
      s[3] = fixed
      $mkxpz_sections[file] = fixed if $mkxpz_sections[file]
      retry
    end
  end
end

# Screen: extended (settings): the PC patcher's widescreen script (its mode
# 2, a wider view on maps; VX / VX Ace), at 5:3 for the top screen (704 at
# 416 high) instead of 16:9. Once: not again after a reset (it aliases).
def mkxpz_widescreen_setup
  return if $mkxpz_ws_loaded || $MKXPZ_RGSS < 2 || !defined?(MKXPZ_WIDESCREEN_SRC)
  $mkxpz_ws_loaded = true
  eval(MKXPZ_WIDESCREEN_SRC, TOPLEVEL_BINDING, 'widescreen.rb')
  MkxpzWidescreen.singleton_class.send(:define_method, :wide_w) { (@base_h * 5.0 / 3 / 32).round * 32 }
rescue StandardError, SyntaxError => e
  mkxpz_log("widescreen: #{e.class}: #{e.message}")
end

# Ruby 1.8 syntax that later Ruby (and MKXP-Z's) rejects, as RPG Maker XP /
# VX scripts have it: "when 3:" (now "when 3 then") and a space before an
# argument list, "Bitmap.new (w, h)" (now a syntax error). Only for a
# section that doesn't compile.
MKXPZ_KEYWORDS = %w[if unless while until and or not when in elsif case then return else do begin end yield
                    defined? super].freeze
def mkxpz_ruby18_fix(code, message = '', file = nil)
  # "next" / "break" / "redo" outside a loop: Ruby 1.8 compiled it (and
  # raised only when it ran there); the lines Ruby names now raise that
  if file
    lines = code.lines
    message.scan(/^#{Regexp.escape(file)}:(\d+): Invalid (next|break|redo|retry)/) do |n, word|
      i = n.to_i - 1
      lines[i] = lines[i].sub(/\b#{word}\b/, "raise(LocalJumpError, '#{word}')") if lines[i]
    end
    code = lines.join
  end
  # (\r: scripts with Windows line ends)
  code = code.gsub(/\bwhen\b([^\n:#]*[^:\s]):(?=[ \t\r]|$)/) { "when#{$1} then" }
  code = code.gsub(/(?<![\w@$:])((?:[A-Za-z_]\w*\.)*[a-z_]\w*[?!]?) +\(([^()\n]*,[^()\n]*)\)/) do
    MKXPZ_KEYWORDS.include?($1) ? $& : "#{$1}(#{$2})"
  end
  # block parameters that are instance or global variables:
  # "do |@infile|" -> "do |mkxpz_p0| @infile = mkxpz_p0;"
  code = code.gsub(/(\bdo[ \t]*|\{[ \t]*)\|([^|\n]*[@$][^|\n]*)\|/) do
    opener, params = $1, $2
    names = params.split(',').map(&:strip)
    temps = names.each_with_index.map { |n, i| n =~ /\A[@$]/ ? "mkxpz_p#{i}" : n }
    sets = names.each_with_index.select { |n, _| n =~ /\A[@$]/ }.map { |n, i| "#{n} = mkxpz_p#{i};" }
    "#{opener}|#{temps.join(', ')}| #{sets.join(' ')}"
  end
  # "x.is_a? (Float)": a predicate's single argument too
  code.gsub(/(?<![\w@$:])((?:[A-Za-z_]\w*\.)*[a-z_]\w*[?!]) +\(([^()\n]*)\)/) do
    MKXPZ_KEYWORDS.include?($1) ? $& : "#{$1}(#{$2})"
  end
end

# "Load the most recent save": games pick it by File.mtime (Changed's save
# screen, VX Ace's DataManager.latest_savefile_index). The 3DS's file times
# are unreliable (emulators: one time for all files), and loading a save
# doesn't change them anyway. So the engine notes when a save file is
# loaded or saved (the standard routines below), and File.mtime /
# File#mtime report that time for it when it's later: after a reset the
# cursor is on the save last used. (In memory: until the app is closed.)
$mkxpz_used ||= {}

class File
  alias_method :mkxpz_real_mtime, :mtime
  def mtime
    t = mkxpz_real_mtime
    u = $mkxpz_used[File.basename(path.to_s)]
    u && u > t ? u : t
  end

  class << self
    alias_method :mkxpz_real_mtime, :mtime
    def mtime(name)
      t = mkxpz_real_mtime(name)
      u = $mkxpz_used[File.basename(name.to_s)]
      u && u > t ? u : t
    end
  end
end

def mkxpz_used_save(name)
  $mkxpz_used[File.basename(name.to_s)] = Time.now if name
rescue StandardError
  nil
end

# wraps the standard save / load routines (again after a reset that ran
# the scripts again: they're the game's fresh methods then)
def mkxpz_used_saves_setup
  if defined?(DataManager) && DataManager.respond_to?(:load_game_without_rescue)
    class << DataManager
      unless method_defined?(:mkxpz_used_load) && instance_method(:load_game_without_rescue).source_location&.first == 'boot.rb'
        alias_method :mkxpz_used_load, :load_game_without_rescue
        def load_game_without_rescue(index)
          r = mkxpz_used_load(index)
          mkxpz_used_save(make_filename(index))
          r
        end
        alias_method :mkxpz_used_save_game, :save_game_without_rescue
        def save_game_without_rescue(index)
          r = mkxpz_used_save_game(index)
          mkxpz_used_save(make_filename(index))
          r
        end
      end
    end
  end
  [[defined?(Scene_File) && Scene_File, :read_save_data], [defined?(Scene_File) && Scene_File, :write_save_data],
   [defined?(Scene_Load) && Scene_Load, :read_save_data], [defined?(Scene_Save) && Scene_Save, :write_save_data]].each do |klass, name|
    next unless klass && klass.method_defined?(name) || (klass && klass.private_method_defined?(name))
    next if mkxpz_ours?(klass, name)
    slow = :"mkxpz_used_#{name}"
    klass.send(:alias_method, slow, name)
    klass.send(:define_method, name) do |file, *rest|
      r = send(slow, file, *rest)
      mkxpz_used_save(file.respond_to?(:path) ? file.path : nil)
      r
    end
  end
rescue StandardError => e
  mkxpz_log("last used save: not followed (#{e.class}: #{e.message})")
end

# A map's character graphics loaded when it's set up (Spriteset_Map.new:
# entering a map, loading a save): every page of every event (pages switch
# during the game, e.g. objects appearing in a boss fight) and the player.
# Big sheets load only the cells of their character (texture pieces), and
# only while there's graphics memory left; the rest as before, when drawn.
def mkxpz_preload_map
  return unless $game_map && $game_map.respond_to?(:events) && defined?(Cache) && Cache.respond_to?(:character)
  t = Time.now
  # (what the last map used is older from now on: room is made from that)
  Graphics.mkxpz_preload_begin if Graphics.respond_to?(:mkxpz_preload_begin)
  seen = {}
  graphics = []
  pictures = []
  faces = []
  # graphics changed by move routes (command 41) and "Change Actor Graphic"
  # (322) in a list of event commands
  sounds = []
  from_route = lambda do |route|
    (route.respond_to?(:list) ? route.list : []).each do |mc|
      graphics << [mc.parameters[0], mc.parameters[1]] if mc.code == 41 && mc.parameters.size >= 2
      sounds << mc.parameters[0] if mc.code == 44 # Play SE
    end
  end
  from_list = lambda do |list|
    (list || []).each do |c|
      case c.code
      when 205 then from_route.call(c.parameters[1])
      when 322 then graphics << [c.parameters[1], c.parameters[2]]
      when 231 then pictures << c.parameters[1] # Show Picture
      when 101 then faces << c.parameters[0]    # Show Text (its face)
      when 250 then sounds << c.parameters[0]   # Play SE
      end
    end
  end
  # first what's on the map now (each event's current graphic, the player):
  # memory may run out before the rest
  graphics << [$game_player.character_name, $game_player.character_index] if $game_player
  $game_map.events.each_value do |ev|
    graphics << [ev.character_name, ev.character_index] if ev.tile_id.to_i == 0
  end
  $game_map.events.each_value do |ev|
    event = ev.instance_variable_get(:@event)
    pages = event.respond_to?(:pages) ? event.pages : []
    pages.each do |pg|
      g = pg.graphic rescue next
      graphics << [g.character_name, g.character_index] if g && g.tile_id.to_i == 0
      from_route.call(pg.move_route) if pg.respond_to?(:move_route)
      from_list.call(pg.list) if pg.respond_to?(:list)
    end
  end
  ($data_common_events || []).each { |ce| from_list.call(ce.list) if ce }
  # sound effects the events play first (small, slow to decode): decoded
  # now, not when first played
  ns = 0
  sounds.map { |se| se.respond_to?(:name) ? se.name : nil }.compact.uniq.each do |name|
    next if name.empty?
    Audio.mkxpz_preload_se("Audio/SE/" + name)
    ns += 1
  end
  n = 0
  graphics.each do |name, index|
    next if !name.is_a?(String) || name.empty? || seen[[name, index]]
    seen[[name, index]] = true
    b = (Cache.character(name) rescue nil) or next
    next if b.disposed?
    sign = name[/^[\!\$]./]
    if sign && sign.include?('$')
      b.mkxpz_preload(0, 0, b.width, b.height)
    else
      cw, ch = b.width / 12, b.height / 8
      b.mkxpz_preload(index % 4 * 3 * cw, index / 4 * 4 * ch, cw * 3, ch * 4)
    end
    n += 1
  end
  # pictures shown by the map's or common events (animations made of
  # pictures): their textures too, while there's room
  np = 0
  pictures.uniq.each do |name|
    next if !name.is_a?(String) || name.empty? || !Cache.respond_to?(:picture)
    b = (Cache.picture(name) rescue nil) or next
    next if b.disposed?
    b.mkxpz_preload(0, 0, b.width, b.height)
    np += 1
  end
  # faces of the map's text boxes: their pixels (copied into the box)
  nf = 0
  faces.uniq.each do |name|
    next if !name.is_a?(String) || name.empty? || !Cache.respond_to?(:face)
    b = (Cache.face(name) rescue nil) or next
    next if b.disposed?
    b.mkxpz_load_pixels if b.respond_to?(:mkxpz_load_pixels)
    nf += 1
  end
  mkxpz_log("preloaded #{n} character graphics, #{np} pictures, #{nf} faces and #{ns} sounds of map #{$game_map.map_id} in %.2f s" % (Time.now - t))
rescue StandardError => e
  mkxpz_log("preload: #{e.class}: #{e.message}")
end

def mkxpz_preload_setup
  return unless defined?(Spriteset_Map) && Spriteset_Map.private_method_defined?(:initialize)
  return if mkxpz_ours?(Spriteset_Map, :initialize)
  Spriteset_Map.class_eval do
    alias_method :mkxpz_preload_initialize, :initialize
    def initialize(*args)
      mkxpz_preload_initialize(*args)
      mkxpz_preload_map
    end
  end
end

def mkxpz_prepare
  mkxpz_preload_setup
  mkxpz_used_saves_setup
  mkxpz_save_screen_setup
  mkxpz_fast_setup unless File.exist?('mkxpz-no-fast')
  mkxpz_fast_setup_events unless File.exist?('mkxpz-no-fast') || File.exist?('mkxpz-no-fast-events')
  mkxpz_wide_setup
  mkxpz_save_probe_setup
end

# Save screens of the "vx新截图存档" script (沉影不器; Changed): each time a
# save file is selected, Window_SaveFile#load_gamedata loads the whole file
# twice (all 15 parts: the game's state, its map and events...) only to
# show the characters, the play time and the screenshot; on the console
# that took seconds per selection. Replaced (while it's that script's
# text) by one that loads the first two parts, steps over the game state
# (Marshal.mkxpz_skip, in C) and loads the screenshot, with the results
# kept per file until the file changes (its size and the checksum of its
# first and last 4 KB; the screenshot sits at the end).
MKXPZ_SAVE_INFO = {}

def mkxpz_save_file_key(path)
  size = File.size(path)
  File.open(path, 'rb') do |f|
    head = f.read(4096).to_s
    f.seek([size - 4096, 0].max)
    [size, Zlib.crc32(head), Zlib.crc32(f.read(4096).to_s)]
  end
end

def mkxpz_save_screen_setup
  return unless defined?(Window_SaveFile) && Window_SaveFile.method_defined?(:load_gamedata) &&
                defined?(SAVE_DIR) && mkxpz_crc_of(Window_SaveFile, :load_gamedata) == 1285622140
  Window_SaveFile.class_eval do
    def load_gamedata
      path = SAVE_DIR + @filename
      @file_exist = FileTest.exist?(path)
      return unless @file_exist
      key = mkxpz_save_file_key(path)
      info = MKXPZ_SAVE_INFO[path]
      unless info && info[0] == key && !(info[3] && info[3].disposed?)
        info = [key]
        File.open(path, 'r') do |file|
          begin
            info << Marshal.load(file) << Marshal.load(file) # characters, frame count
            12.times { Marshal.mkxpz_skip(file) }            # the game's state
            info << (begin; Marshal.load(file); rescue StandardError; nil; end) # the screenshot
            info << true << info[2] / Graphics.frame_rate
          rescue StandardError
            info = [key, nil, nil, nil, false, nil]
          end
        end
        MKXPZ_SAVE_INFO[path] = info
      end
      @file_exist = info[4]
      return unless @file_exist
      @characters, @frame_count, @file_bitmap, @total_sec = info[1], info[2], info[3], info[5]
    end
  end
  mkxpz_log('save screen: quick (screenshot save script)')
rescue StandardError => e
  mkxpz_log("save screen: as it is (#{e.class}: #{e.message})")
end

def mkxpz_late_setup
  mkxpz_log("scripts from a loader: prepared at the first frame")
  mkxpz_prepare
  eval(File.read('mkxpz-test.rb'), TOPLEVEL_BINDING, 'mkxpz-test.rb') if $mkxpz_late_test
end
