# Added by the MKXP-Z Patcher.
# Steam games: when the file mkxp-z/steam-log.txt exists (create an empty one),
# or the environment variable MKXPZ_STEAM_LOG is set, every call of the game's
# scripts into steam_api.dll is added to that file with its arguments and
# result, e.g. to check that achievements reach Steam:
#   SteamAPI_ISteamUserStats_SetAchievement(..., "ACH_NAME") -> 1
#   SteamAPI_ISteamUserStats_StoreStats(...) -> 1
# MKXPZ_STEAM_LOG=<name> logs the DLLs whose names start with <name> instead
# (e.g. kernel32). Without the file or the variable, nothing changes.

module MkxpzSteamLog
  FILE = 'mkxp-z/steam-log.txt'
  want = ENV['MKXPZ_STEAM_LOG'].to_s.strip
  want = '1' if want.empty? && File.exist?(FILE)
  PREFIX = want.empty? ? nil : (want == '1' ? 'steam_api' : want).downcase
end

if MkxpzSteamLog::PREFIX && defined?(Win32API)
  module MkxpzSteamLog

    def self.write(line)
      File.open(FILE, 'a') { |f| f.puts("#{Time.now.strftime('%H:%M:%S')} #{line}") }
    rescue
      nil
    end

    def self.show(v)
      v.is_a?(String) ? v.unpack('Z*')[0].to_s[0, 80].inspect : v.inspect
    end

    write("--- #{Time.now} log of #{PREFIX}* calls")
  end

  class Win32API
    alias_method :mkxpz_steamlog_initialize, :initialize
    def initialize(dll, func, *rest)
      if dll.to_s.downcase.start_with?(MkxpzSteamLog::PREFIX)
        @mkxpz_steamlog = func.to_s
        MkxpzSteamLog.write("load #{dll}:#{func}")
      end
      mkxpz_steamlog_initialize(dll, func, *rest)
      # win32_wrap.rb hides load errors (every call then returns 0)
      if @mkxpz_steamlog && instance_variable_defined?(:@mkxp_native_available) && !@mkxp_native_available
        MkxpzSteamLog.write("  #{dll}:#{func} could NOT be loaded: its calls return 0")
      end
    rescue Exception => e
      MkxpzSteamLog.write("load #{dll}:#{func} FAILED: #{e.class}: #{e.message}") if @mkxpz_steamlog
      raise
    end

    alias_method :mkxpz_steamlog_call, :call
    def call(*args)
      return mkxpz_steamlog_call(*args) unless @mkxpz_steamlog
      begin
        r = mkxpz_steamlog_call(*args)
      rescue Exception => e
        MkxpzSteamLog.write("#{@mkxpz_steamlog}(#{args.map { |a| MkxpzSteamLog.show(a) }.join(', ')}) " \
                            "raised #{e.class}: #{e.message}")
        raise
      end
      # RunCallbacks runs every frame: log it once
      unless @mkxpz_steamlog.end_with?('RunCallbacks') && @mkxpz_steamlog_seen
        MkxpzSteamLog.write("#{@mkxpz_steamlog}(#{args.map { |a| MkxpzSteamLog.show(a) }.join(', ')}) -> #{r.inspect}")
        @mkxpz_steamlog_seen = true
      end
      r
    end
    alias_method :Call, :call
  end
end
