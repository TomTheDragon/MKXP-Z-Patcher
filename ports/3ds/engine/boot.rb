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

# Windows functions: none on the 3DS; calls do nothing
class Win32API
  def initialize(*) end
  def call(*) 0 end
  alias Call call
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
# XP / VX print with message boxes too
alias print msgbox if $MKXPZ_RGSS >= 2

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
    m.original_name == :update && m.owner == Game_Event && mkxpz_stock?(Game_Event, n)
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
      return if @mkxpz_m == m || (@mkxpz_m && openness > 0) || disposed?
      old = @mkxpz_m || 0
      @mkxpz_m = m
      self.mkxpz_wide = true
      self.x = x + old - m
      self.width = width - 2 * old + 2 * m
      create_contents
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
  $RGSS_SCRIPTS = load_data($MKXPZ_SCRIPTS)
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
    eval(s[3], TOPLEVEL_BINDING, name.empty? ? "Section#{i}" : name)
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

def mkxpz_prepare
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
