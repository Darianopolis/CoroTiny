if Project "corotiny" then
    Compile "src/**"
    Include "src"
    Artifact { "out/main", type = "Console" }
end